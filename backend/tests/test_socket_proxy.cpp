/**
 * Socket 本地代理 (serve/request 模式) Linux 集成测试
 *
 * 与单元风格的其他测试不同，本套件真正以独立进程启动：
 *   - `ipc_demo serve`    作为服务端进程
 *   - `ipc_demo request`  作为多个客户端进程
 * 通过退出码与日志输出观察并断言：
 *   - 多客户端并发响应、慢请求/慢消费者不阻塞其他连接
 *   - 拆包 (逐字节写入) 与粘包 (一次写入多帧) 的正确处理
 *   - 帧长超限、空闲超时、协议字段非法时连接被关闭且原因可区分
 *   - 失效 Socket 清理、非 Socket 占用拒绝、权限 0600
 *   - SIGTERM 优雅退出 (退出码 0) 与宽限期超期 (退出码 3)
 */

#include "test_framework.h"
#include "socket_proto.h"

#include <poll.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <vector>

using namespace test;
using namespace std::chrono;

namespace {

std::string g_dir;  // 本套件的独立运行目录 (/tmp/ipc_proxy_test_<pid>)

// 定位 ipc_demo 可执行文件: 环境变量 > 编译期注入路径 > 常见位置
std::string demo_bin() {
    const char* env = getenv("IPC_DEMO_BIN");
    if (env && *env) return env;
#ifdef IPC_DEMO_BIN
    if (access(IPC_DEMO_BIN, X_OK) == 0) return IPC_DEMO_BIN;
#endif
    for (const char* p : {"./ipc_demo", "../ipc_demo", "/app/ipc_demo"}) {
        if (access(p, X_OK) == 0) return p;
    }
    return "ipc_demo";  // 交给 PATH 查找
}

long now_ms() {
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool file_contains(const std::string& path, const std::string& needle) {
    return read_file(path).find(needle) != std::string::npos;
}

bool wait_for_log(const std::string& path, const std::string& needle, int timeout_ms) {
    long deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        if (file_contains(path, needle)) return true;
        usleep(20000);
    }
    return file_contains(path, needle);
}

// 测试失败时清理仍在运行的子进程
struct ChildGuard {
    std::vector<pid_t> pids;
    ~ChildGuard() {
        for (pid_t p : pids) {
            kill(p, SIGKILL);
            waitpid(p, nullptr, 0);
        }
    }
    void add(pid_t p) { pids.push_back(p); }
    void remove(pid_t p) {
        pids.erase(std::remove(pids.begin(), pids.end(), p), pids.end());
    }
};

// 以独立进程启动 ipc_demo，stdout/stderr 重定向到日志文件
pid_t spawn(const std::string& log_name, const std::vector<std::string>& args,
            std::string& log_out) {
    log_out = g_dir + "/" + log_name + ".log";
    std::string bin = demo_bin();
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(log_out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) close(fd);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(bin.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(bin.c_str(), argv.data());
        _exit(127);
    }
    return pid;
}

// 等待子进程退出，返回退出码；超时则 SIGKILL 并返回 -1
int wait_exit(pid_t pid, int timeout_ms) {
    int status = 0;
    long deadline = now_ms() + timeout_ms;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return -2;
        }
        if (now_ms() >= deadline) break;
        usleep(10000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}

// 启动服务端进程并等待其输出 listening 就绪日志
pid_t start_server(ChildGuard& guard, const std::string& name, const std::string& sock,
                   const std::vector<std::string>& extra, std::string& log_out) {
    std::vector<std::string> args = {"serve", "--path", sock};
    args.insert(args.end(), extra.begin(), extra.end());
    pid_t pid = spawn(name, args, log_out);
    guard.add(pid);
    return pid;
}

// 启动客户端进程
pid_t start_client(ChildGuard& guard, const std::string& name, const std::string& sock,
                   const std::vector<std::string>& extra, std::string& log_out) {
    std::vector<std::string> args = {"request", "--path", sock};
    args.insert(args.end(), extra.begin(), extra.end());
    pid_t pid = spawn(name, args, log_out);
    guard.add(pid);
    return pid;
}

// 测试内直接发起的裸连接 (用于协议错误/半包断开等场景)
int raw_connect(const std::string& sock) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// 向服务端发 SIGTERM 并断言其按预期退出码退出、Socket 文件被清理
void assert_server_shutdown(pid_t srv, const std::string& sock,
                            int expect_code) {
    ASSERT_EQ(kill(srv, SIGTERM), 0);
    int rc = wait_exit(srv, 6000);
    ASSERT_EQ(rc, expect_code);
    struct stat st {};
    ASSERT_EQ(stat(sock.c_str(), &st), -1);
    ASSERT_EQ(errno, ENOENT);
}

} // namespace

// ---------------------------------------------------------------------------
// 基础: 可执行文件存在、协议头大端编码正确
// ---------------------------------------------------------------------------
void test_proxy_binary_exists() {
    ASSERT_EQ(access(demo_bin().c_str(), X_OK), 0);
}

void test_proxy_proto_header() {
    auto frame = ipc::proto::build_frame(ipc::proto::TYPE_REQUEST, 0x01020304u, "hello");
    ASSERT_EQ(frame.size(), ipc::proto::HEADER_SIZE + 5);
    ASSERT_EQ(frame[0], (uint8_t)'I');
    ASSERT_EQ(frame[1], (uint8_t)'P');
    ASSERT_EQ(frame[2], ipc::proto::VERSION);
    ASSERT_EQ(frame[3], ipc::proto::TYPE_REQUEST);
    // request_id 大端序: 01 02 03 04
    ASSERT_EQ(frame[4], (uint8_t)0x01);
    ASSERT_EQ(frame[5], (uint8_t)0x02);
    ASSERT_EQ(frame[6], (uint8_t)0x03);
    ASSERT_EQ(frame[7], (uint8_t)0x04);
    ASSERT_EQ(ipc::proto::get_u32be(frame.data() + 4), 0x01020304u);
    ASSERT_EQ(ipc::proto::get_u32be(frame.data() + 8), 5u);
    ASSERT_EQ(std::string((char*)frame.data() + ipc::proto::HEADER_SIZE, 5), "hello");
}

// ---------------------------------------------------------------------------
// 并发: 多个客户端同时工作，慢请求不阻塞其他连接；权限 0600；优雅退出
// ---------------------------------------------------------------------------
void test_proxy_concurrent_clients() {
    ChildGuard guard;
    std::string sock = g_dir + "/concurrent.sock";
    std::string srv_log;
    pid_t srv = start_server(guard, "srv_concurrent", sock,
        {"--max-frame", "1024", "--idle-timeout", "10", "--grace", "5"}, srv_log);
    ASSERT_TRUE(wait_for_log(srv_log, "listening on", 5000));

    // Socket 文件权限: 仅当前用户可读写 (0600)
    struct stat st {};
    ASSERT_EQ(stat(sock.c_str(), &st), 0);
    ASSERT_TRUE(S_ISSOCK(st.st_mode));
    ASSERT_EQ((int)(st.st_mode & 0777), 0600);

    // 慢客户端: 唯一请求需服务端处理 900ms
    std::string logA, logB, logC;
    pid_t a = start_client(guard, "cli_slow", sock,
        {"--client-id", "slow", "--sleep-ms", "900", "--count", "1"}, logA);
    usleep(150000);
    // 两个快客户端在慢请求处理期间进入
    pid_t b = start_client(guard, "cli_fast1", sock,
        {"--client-id", "fast1", "--count", "5"}, logB);
    pid_t c = start_client(guard, "cli_fast2", sock,
        {"--client-id", "fast2", "--count", "5", "--pipeline"}, logC);

    int rcB = wait_exit(b, 8000); long tB = now_ms(); guard.remove(b);
    int rcC = wait_exit(c, 8000); long tC = now_ms(); guard.remove(c);
    int rcA = wait_exit(a, 8000); long tA = now_ms(); guard.remove(a);

    ASSERT_EQ(rcB, 0);
    ASSERT_EQ(rcC, 0);
    ASSERT_EQ(rcA, 0);
    // 快客户端在慢请求完成之前就已全部结束
    ASSERT_TRUE(tB < tA);
    ASSERT_TRUE(tC < tA);
    // 客户端自身校验了响应顺序
    ASSERT_TRUE(file_contains(logA, "in request order"));
    ASSERT_TRUE(file_contains(logB, "in request order"));
    ASSERT_TRUE(file_contains(logC, "in request order"));
    // 服务端确实同时维护了 3 个连接
    ASSERT_TRUE(file_contains(srv_log, "conn#3 accepted"));

    assert_server_shutdown(srv, sock, 0);
    guard.remove(srv);
    ASSERT_TRUE(file_contains(srv_log, "shutdown complete"));
}

// ---------------------------------------------------------------------------
// 拆包: 逐字节写入也能正确重组
// ---------------------------------------------------------------------------
void test_proxy_fragmented_frames() {
    ChildGuard guard;
    std::string sock = g_dir + "/fragment.sock";
    std::string srv_log;
    pid_t srv = start_server(guard, "srv_fragment", sock, {"--grace", "2"}, srv_log);
    ASSERT_TRUE(wait_for_log(srv_log, "listening on", 5000));

    std::string cli_log;
    pid_t cli = start_client(guard, "cli_fragment", sock,
        {"--count", "3", "--chunk-bytes", "1", "--chunk-delay-ms", "2"}, cli_log);
    int rc = wait_exit(cli, 10000);
    guard.remove(cli);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(file_contains(cli_log, "in request order"));

    assert_server_shutdown(srv, sock, 0);
    guard.remove(srv);
}

// ---------------------------------------------------------------------------
// 粘包 + 顺序: 一次写入多帧；首个请求延迟时后续响应仍按请求顺序返回
// ---------------------------------------------------------------------------
void test_proxy_coalesced_frames_ordered() {
    ChildGuard guard;
    std::string sock = g_dir + "/coalesce.sock";
    std::string srv_log;
    pid_t srv = start_server(guard, "srv_coalesce", sock, {"--grace", "3"}, srv_log);
    ASSERT_TRUE(wait_for_log(srv_log, "listening on", 5000));

    std::string cli_log;
    long t0 = now_ms();
    pid_t cli = start_client(guard, "cli_coalesce", sock,
        {"--pipeline", "--count", "4", "--sleep-ms", "400"}, cli_log);
    int rc = wait_exit(cli, 10000);
    long elapsed = now_ms() - t0;
    guard.remove(cli);
    ASSERT_EQ(rc, 0);
    // 首个请求延迟 400ms，后续响应必须排在它后面，总耗时不低于该延迟
    ASSERT_TRUE(elapsed >= 350);
    ASSERT_TRUE(file_contains(cli_log, "in request order"));

    assert_server_shutdown(srv, sock, 0);
    guard.remove(srv);
}

// ---------------------------------------------------------------------------
// 慢消费者: 大响应 + 慢速读取 (触发服务端短写/发送缓冲)，不阻塞其他连接
// ---------------------------------------------------------------------------
void test_proxy_slow_reader_not_blocking() {
    ChildGuard guard;
    std::string sock = g_dir + "/slowread.sock";
    std::string srv_log;
    pid_t srv = start_server(guard, "srv_slowread", sock,
        {"--max-frame", "1048576", "--grace", "5"}, srv_log);
    ASSERT_TRUE(wait_for_log(srv_log, "listening on", 5000));

    // 慢消费者: 512KB 响应，每次只读 4KB 并间隔 3ms
    std::string logA, logB;
    pid_t a = start_client(guard, "cli_slowreader", sock,
        {"--client-id", "slowread", "--payload-size", "524288", "--count", "1",
         "--read-chunk", "4096", "--read-delay-ms", "3", "--recv-timeout", "20000"}, logA);
    usleep(200000);
    pid_t b = start_client(guard, "cli_normal", sock,
        {"--client-id", "normal", "--count", "5"}, logB);

    int rcB = wait_exit(b, 8000); long tB = now_ms(); guard.remove(b);
    int rcA = wait_exit(a, 20000); long tA = now_ms(); guard.remove(a);

    ASSERT_EQ(rcB, 0);
    ASSERT_EQ(rcA, 0);
    ASSERT_TRUE(tB < tA);  // 慢消费者没有拖住其他连接

    assert_server_shutdown(srv, sock, 0);
    guard.remove(srv);
}

// ---------------------------------------------------------------------------
// 帧长超限: 连接被关闭且原因可区分，服务端继续服务其他客户端
// ---------------------------------------------------------------------------
void test_proxy_oversize_frame_rejected() {
    ChildGuard guard;
    std::string sock = g_dir + "/oversize.sock";
    std::string srv_log;
    pid_t srv = start_server(guard, "srv_oversize", sock,
        {"--max-frame", "64", "--grace", "2"}, srv_log);
    ASSERT_TRUE(wait_for_log(srv_log, "listening on", 5000));

    // 发送声明 65 字节 (> 上限 64) 的帧头
    std::string logJumbo, logNormal;
    pid_t jumbo = start_client(guard, "cli_jumbo", sock, {"--jumbo", "65"}, logJumbo);
    int rc = wait_exit(jumbo, 8000);
    guard.remove(jumbo);
    ASSERT_EQ(rc, 4);  // REQ_ERR_CLOSED_BY_SERVER
    ASSERT_TRUE(wait_for_log(srv_log, "reason=frame-too-large", 3000));

    // 服务端仍正常服务其他客户端
    pid_t normal = start_client(guard, "cli_after_oversize", sock, {"--count", "3"}, logNormal);
    rc = wait_exit(normal, 8000);
    guard.remove(normal);
    ASSERT_EQ(rc, 0);

    assert_server_shutdown(srv, sock, 0);
    guard.remove(srv);
}

// ---------------------------------------------------------------------------
// 空闲超时: 沉默连接被关闭并记录原因
// ---------------------------------------------------------------------------
void test_proxy_idle_timeout_close() {
    ChildGuard guard;
    std::string sock = g_dir + "/idle.sock";
    std::string srv_log;
    pid_t srv = start_server(guard, "srv_idle", sock,
        {"--idle-timeout", "1", "--grace", "2"}, srv_log);
    ASSERT_TRUE(wait_for_log(srv_log, "listening on", 5000));

    std::string cli_log;
    pid_t cli = start_client(guard, "cli_idle", sock, {"--hold", "3"}, cli_log);
    int rc = wait_exit(cli, 8000);
    guard.remove(cli);
    ASSERT_EQ(rc, 4);  // 被服务端关闭
    ASSERT_TRUE(wait_for_log(srv_log, "reason=idle-timeout", 3000));

    assert_server_shutdown(srv, sock, 0);
    guard.remove(srv);
}

// ---------------------------------------------------------------------------
// 协议字段非法 + 半包后中途断开
// ---------------------------------------------------------------------------
void test_proxy_protocol_error_and_disconnect() {
    ChildGuard guard;
    std::string sock = g_dir + "/protoerr.sock";
    std::string srv_log;
    pid_t srv = start_server(guard, "srv_protoerr", sock, {"--grace", "2"}, srv_log);
    ASSERT_TRUE(wait_for_log(srv_log, "listening on", 5000));

    // 1) 发送非法 magic 的垃圾数据，服务端应关闭连接并记录 protocol-error
    int fd = raw_connect(sock);
    ASSERT_NE(fd, -1);
    const char garbage[] = "GARBAGEGARBAGE!";  // 16 字节，足以构成一个“头”
    ASSERT_EQ(write(fd, garbage, sizeof(garbage)), (ssize_t)sizeof(garbage));
    pollfd pfd {fd, POLLIN, 0};
    ASSERT_EQ(poll(&pfd, 1, 2000), 1);
    char tmp[16];
    ASSERT_EQ(read(fd, tmp, sizeof(tmp)), 0);  // 连接已被服务端关闭
    close(fd);
    ASSERT_TRUE(wait_for_log(srv_log, "reason=protocol-error", 3000));

    // 2) 只发半个帧头就断开，服务端应记录 client-disconnect 与半包丢弃
    fd = raw_connect(sock);
    ASSERT_NE(fd, -1);
    auto half = ipc::proto::build_frame(ipc::proto::TYPE_REQUEST, 1, "never-finished");
    ASSERT_EQ(write(fd, half.data(), 6), (ssize_t)6);  // 不足 12 字节头
    close(fd);
    ASSERT_TRUE(wait_for_log(srv_log, "reason=client-disconnect", 3000));
    ASSERT_TRUE(wait_for_log(srv_log, "incomplete frame", 3000));

    // 3) 服务端仍正常服务其他客户端
    std::string cli_log;
    pid_t cli = start_client(guard, "cli_after_protoerr", sock, {"--count", "2"}, cli_log);
    int rc = wait_exit(cli, 8000);
    guard.remove(cli);
    ASSERT_EQ(rc, 0);

    assert_server_shutdown(srv, sock, 0);
    guard.remove(srv);
}

// ---------------------------------------------------------------------------
// 优雅退出: SIGTERM 后停止接收新连接，在途请求完成后清理退出
// ---------------------------------------------------------------------------
void test_proxy_graceful_shutdown() {
    ChildGuard guard;
    std::string sock = g_dir + "/grace.sock";
    std::string srv_log;
    pid_t srv = start_server(guard, "srv_grace", sock, {"--grace", "5"}, srv_log);
    ASSERT_TRUE(wait_for_log(srv_log, "listening on", 5000));

    // 在途慢请求: 1200ms 后才完成
    std::string cli_log;
    pid_t cli = start_client(guard, "cli_grace", sock,
        {"--sleep-ms", "1200", "--count", "1"}, cli_log);
    ASSERT_TRUE(wait_for_log(srv_log, "deferred 1200ms", 3000));

    ASSERT_EQ(kill(srv, SIGTERM), 0);
    ASSERT_TRUE(wait_for_log(srv_log, "draining", 3000));

    // 新连接被拒绝 (listen 已关闭)
    int fd = raw_connect(sock);
    ASSERT_EQ(fd, -1);

    // 在途请求在宽限期内完成，客户端正常拿到响应
    int rc = wait_exit(cli, 8000);
    guard.remove(cli);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(file_contains(cli_log, "in request order"));

    // 服务端优雅退出并清理 Socket 文件
    rc = wait_exit(srv, 6000);
    guard.remove(srv);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(file_contains(srv_log, "shutdown complete"));
    struct stat st {};
    ASSERT_EQ(stat(sock.c_str(), &st), -1);
    ASSERT_EQ(errno, ENOENT);
}

// ---------------------------------------------------------------------------
// 宽限期超期: 在途请求无法完成时明确失败 (退出码 3)
// ---------------------------------------------------------------------------
void test_proxy_grace_expired_fails() {
    ChildGuard guard;
    std::string sock = g_dir + "/graceexp.sock";
    std::string srv_log;
    pid_t srv = start_server(guard, "srv_graceexp", sock, {"--grace", "1"}, srv_log);
    ASSERT_TRUE(wait_for_log(srv_log, "listening on", 5000));

    // 慢请求需要 4000ms，远超 1s 宽限期
    std::string cli_log;
    pid_t cli = start_client(guard, "cli_graceexp", sock,
        {"--sleep-ms", "4000", "--count", "1", "--recv-timeout", "15000"}, cli_log);
    ASSERT_TRUE(wait_for_log(srv_log, "deferred 4000ms", 3000));

    ASSERT_EQ(kill(srv, SIGTERM), 0);

    // 服务端在宽限期超期后明确失败
    int rc = wait_exit(srv, 6000);
    guard.remove(srv);
    ASSERT_EQ(rc, 3);  // SERVE_ERR_GRACE_EXPIRED
    ASSERT_TRUE(file_contains(srv_log, "grace period expired"));
    struct stat st {};
    ASSERT_EQ(stat(sock.c_str(), &st), -1);  // Socket 文件仍被清理
    ASSERT_EQ(errno, ENOENT);

    // 客户端观察到连接被关闭
    rc = wait_exit(cli, 8000);
    guard.remove(cli);
    ASSERT_EQ(rc, 4);
}

// ---------------------------------------------------------------------------
// 失效 Socket: 确认失效后移除并正常启动
// ---------------------------------------------------------------------------
void test_proxy_stale_socket_removed() {
    ChildGuard guard;
    std::string sock = g_dir + "/stale.sock";

    // 制造失效 Socket: bind 后关闭进程端 fd，文件残留且无监听者
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_NE(fd, -1);
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
    ASSERT_EQ(bind(fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
    close(fd);
    struct stat st {};
    ASSERT_EQ(stat(sock.c_str(), &st), 0);
    ASSERT_TRUE(S_ISSOCK(st.st_mode));

    // 服务端应识别失效 Socket、移除并正常启动
    std::string srv_log;
    pid_t srv = start_server(guard, "srv_stale", sock, {"--grace", "2"}, srv_log);
    ASSERT_TRUE(wait_for_log(srv_log, "listening on", 5000));
    ASSERT_TRUE(file_contains(srv_log, "stale socket"));

    assert_server_shutdown(srv, sock, 0);
    guard.remove(srv);
}

// ---------------------------------------------------------------------------
// 路径被非 Socket 节点占用: 拒绝移除，退出码 2
// ---------------------------------------------------------------------------
void test_proxy_path_not_socket_refused() {
    ChildGuard guard;
    std::string sock = g_dir + "/notasocket.sock";

    // 普通文件占用路径
    int fd = open(sock.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_NE(fd, -1);
    ASSERT_EQ(write(fd, "x", 1), 1);
    close(fd);

    std::string srv_log;
    pid_t srv = start_server(guard, "srv_notasocket", sock, {}, srv_log);
    int rc = wait_exit(srv, 5000);
    guard.remove(srv);
    ASSERT_EQ(rc, 2);  // SERVE_ERR_PATH_BUSY
    ASSERT_TRUE(file_contains(srv_log, "not a socket"));
    // 文件未被删除
    struct stat st {};
    ASSERT_EQ(stat(sock.c_str(), &st), 0);
    unlink(sock.c_str());
}

// ---------------------------------------------------------------------------
// 路径被活跃服务端占用: 拒绝启动，退出码 2
// ---------------------------------------------------------------------------
void test_proxy_live_server_conflict() {
    ChildGuard guard;
    std::string sock = g_dir + "/live.sock";
    std::string srv_log;
    pid_t srv = start_server(guard, "srv_live", sock, {"--grace", "2"}, srv_log);
    ASSERT_TRUE(wait_for_log(srv_log, "listening on", 5000));

    // 第二个服务端尝试同一路径，应失败
    std::string srv2_log;
    pid_t srv2 = start_server(guard, "srv_live2", sock, {}, srv2_log);
    int rc = wait_exit(srv2, 5000);
    guard.remove(srv2);
    ASSERT_EQ(rc, 2);
    ASSERT_TRUE(file_contains(srv2_log, "already in use"));

    // 第一个服务端不受影响，正常优雅退出
    assert_server_shutdown(srv, sock, 0);
    guard.remove(srv);
}

void test_socket_proxy_suite() {
    g_dir = "/tmp/ipc_proxy_test_" + std::to_string(getpid());
    mkdir(g_dir.c_str(), 0700);
    std::cout << "  (proxy 测试日志目录: " << g_dir << ")" << std::endl;

    run_test("proxy_binary_exists", test_proxy_binary_exists);
    run_test("proxy_proto_header", test_proxy_proto_header);
    run_test("proxy_concurrent_clients", test_proxy_concurrent_clients);
    run_test("proxy_fragmented_frames", test_proxy_fragmented_frames);
    run_test("proxy_coalesced_frames_ordered", test_proxy_coalesced_frames_ordered);
    run_test("proxy_slow_reader_not_blocking", test_proxy_slow_reader_not_blocking);
    run_test("proxy_oversize_frame_rejected", test_proxy_oversize_frame_rejected);
    run_test("proxy_idle_timeout_close", test_proxy_idle_timeout_close);
    run_test("proxy_protocol_error_and_disconnect", test_proxy_protocol_error_and_disconnect);
    run_test("proxy_graceful_shutdown", test_proxy_graceful_shutdown);
    run_test("proxy_grace_expired_fails", test_proxy_grace_expired_fails);
    run_test("proxy_stale_socket_removed", test_proxy_stale_socket_removed);
    run_test("proxy_path_not_socket_refused", test_proxy_path_not_socket_refused);
    run_test("proxy_live_server_conflict", test_proxy_live_server_conflict);
}
