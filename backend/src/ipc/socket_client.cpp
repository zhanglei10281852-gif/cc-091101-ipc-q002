/**
 * Unix Domain Socket 本地代理 - 客户端 (request 模式)
 *
 * 作为独立进程连接 serve 服务端，发送一组请求并校验响应：
 * - 校验响应 request_id 与请求顺序一致、内容与预期相符
 * - 支持 pipeline (一次写入全部帧，制造粘包) 与按块写入 (制造拆包)
 * - 支持慢速读取 (模拟慢消费者)、hold (空闲连接)、jumbo (超限帧头) 模式
 * - 退出码可区分: 连接失败 / I/O 错误 / 被服务端关闭 / 校验失败 / 超时
 */

#include "socket_proxy.h"
#include "socket_proto.h"
#include "ipc_demo.h"

#include <poll.h>

#include <chrono>
#include <stdexcept>
#include <vector>

namespace ipc {
namespace {

using Clock = std::chrono::steady_clock;

// 发送全部字节，正确处理短写；chunk>0 时按块发送 (模拟拆包)
int send_all(int fd, const uint8_t* data, std::size_t size, int chunk, int delay_ms,
             const std::string& tag) {
    std::size_t off = 0;
    while (off < size) {
        std::size_t want = size - off;
        if (chunk > 0 && want > static_cast<std::size_t>(chunk)) {
            want = static_cast<std::size_t>(chunk);
        }
        ssize_t n = ::send(fd, data + off, want, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE || errno == ECONNRESET) {
                Logger::error(tag, "connection closed by server while sending");
                return REQ_ERR_CLOSED_BY_SERVER;
            }
            Logger::error(tag, std::string("send 失败: ") + strerror(errno));
            return REQ_ERR_IO;
        }
        off += static_cast<std::size_t>(n);  // 短写: 继续发送剩余部分
        if (off < size && delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
    return REQ_OK;
}

// 在 deadline 前精确接收 n 字节；read_chunk>0 时按块读取 (模拟慢消费者)
int recv_exact(int fd, uint8_t* buf, std::size_t n, Clock::time_point deadline,
               const RequestConfig& cfg, const std::string& tag) {
    std::size_t got = 0;
    while (got < n) {
        auto now = Clock::now();
        if (now >= deadline) {
            Logger::error(tag, "recv timeout");
            return REQ_ERR_TIMEOUT;
        }
        int remain = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (remain <= 0) remain = 1;
        pollfd pfd {fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, remain);
        if (r < 0) {
            if (errno == EINTR) continue;
            Logger::error(tag, std::string("poll 失败: ") + strerror(errno));
            return REQ_ERR_IO;
        }
        if (r == 0) {
            Logger::error(tag, "recv timeout");
            return REQ_ERR_TIMEOUT;
        }
        std::size_t want = n - got;
        if (cfg.read_chunk_bytes > 0 && want > static_cast<std::size_t>(cfg.read_chunk_bytes)) {
            want = static_cast<std::size_t>(cfg.read_chunk_bytes);
        }
        ssize_t k = ::read(fd, buf + got, want);
        if (k == 0) {
            Logger::error(tag, "connection closed by server");
            return REQ_ERR_CLOSED_BY_SERVER;
        }
        if (k < 0) {
            if (errno == EINTR) continue;
            Logger::error(tag, std::string("read 失败: ") + strerror(errno));
            return REQ_ERR_IO;
        }
        got += static_cast<std::size_t>(k);
        if (got < n && cfg.read_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.read_delay_ms));
        }
    }
    return REQ_OK;
}

// 接收一个完整响应帧
int recv_frame(int fd, uint32_t& request_id, std::string& payload, int timeout_ms,
               const RequestConfig& cfg, const std::string& tag) {
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    uint8_t hdr[proto::HEADER_SIZE];
    int rc = recv_exact(fd, hdr, sizeof(hdr), deadline, cfg, tag);
    if (rc != REQ_OK) return rc;
    if (hdr[0] != proto::MAGIC0 || hdr[1] != proto::MAGIC1) {
        Logger::error(tag, "invalid response magic");
        return REQ_ERR_MISMATCH;
    }
    if (hdr[2] != proto::VERSION) {
        Logger::error(tag, "unsupported response version " + std::to_string(hdr[2]));
        return REQ_ERR_MISMATCH;
    }
    if (hdr[3] != proto::TYPE_RESPONSE) {
        Logger::error(tag, "unexpected frame type " + std::to_string(hdr[3]));
        return REQ_ERR_MISMATCH;
    }
    request_id = proto::get_u32be(hdr + 4);
    uint32_t len = proto::get_u32be(hdr + 8);
    if (len > 64u * 1024 * 1024) {
        Logger::error(tag, "response len absurd: " + std::to_string(len));
        return REQ_ERR_MISMATCH;
    }
    payload.resize(len);
    if (len > 0) {
        rc = recv_exact(fd, reinterpret_cast<uint8_t*>(&payload[0]), len, deadline, cfg, tag);
        if (rc != REQ_OK) return rc;
    }
    return REQ_OK;
}

std::string preview(const std::string& payload) {
    if (payload.size() <= 60) return payload;
    return payload.substr(0, 57) + "...";
}

// 正常模式: 发送 count 个请求并逐个校验响应 (顺序 + 内容)
int normal_mode(int fd, const RequestConfig& cfg, const std::string& tag) {
    std::string msg = cfg.payload_size > 0
                          ? std::string(static_cast<std::size_t>(cfg.payload_size), 'x')
                          : cfg.message;

    std::vector<std::string> payloads(static_cast<std::size_t>(cfg.count));
    std::vector<std::string> expected(static_cast<std::size_t>(cfg.count));
    for (int i = 0; i < cfg.count; ++i) {
        if (cfg.sleep_ms >= 0 && i == 0) {
            // 首个请求要求服务端延迟响应，验证后续响应不会越过它先返回
            payloads[static_cast<std::size_t>(i)] =
                "SLEEP:" + std::to_string(cfg.sleep_ms) + ":" + msg;
            expected[static_cast<std::size_t>(i)] = "OK:" + msg;
        } else {
            payloads[static_cast<std::size_t>(i)] = msg;
            expected[static_cast<std::size_t>(i)] = "ECHO:" + msg;
        }
    }

    auto send_one = [&](int i) -> int {
        std::vector<uint8_t> frame = proto::build_frame(
            proto::TYPE_REQUEST, static_cast<uint32_t>(i + 1),
            payloads[static_cast<std::size_t>(i)]);
        int rc = send_all(fd, frame.data(), frame.size(), cfg.chunk_bytes,
                          cfg.chunk_delay_ms, tag);
        if (rc == REQ_OK) {
            Logger::info(tag, "sent request id=" + std::to_string(i + 1) + " len=" +
                         std::to_string(payloads[static_cast<std::size_t>(i)].size()));
        }
        return rc;
    };

    auto recv_one = [&](int i) -> int {
        uint32_t rid = 0;
        std::string payload;
        auto t0 = Clock::now();
        int rc = recv_frame(fd, rid, payload, cfg.recv_timeout_ms, cfg, tag);
        if (rc != REQ_OK) return rc;
        long rtt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        if (rid != static_cast<uint32_t>(i + 1)) {
            Logger::error(tag, "response out of order: got id=" + std::to_string(rid) +
                          " expect id=" + std::to_string(i + 1));
            return REQ_ERR_MISMATCH;
        }
        if (payload != expected[static_cast<std::size_t>(i)]) {
            Logger::error(tag, "payload mismatch for id=" + std::to_string(rid));
            return REQ_ERR_MISMATCH;
        }
        Logger::info(tag, "response id=" + std::to_string(rid) + " verified len=" +
                     std::to_string(payload.size()) + " rtt=" + std::to_string(rtt) +
                     "ms: " + preview(payload));
        return REQ_OK;
    };

    if (cfg.pipeline) {
        // 粘包: 全部帧拼接成一个流一次写入
        std::vector<uint8_t> blob;
        for (int i = 0; i < cfg.count; ++i) {
            std::vector<uint8_t> frame = proto::build_frame(
                proto::TYPE_REQUEST, static_cast<uint32_t>(i + 1),
                payloads[static_cast<std::size_t>(i)]);
            blob.insert(blob.end(), frame.begin(), frame.end());
        }
        int rc = send_all(fd, blob.data(), blob.size(), cfg.chunk_bytes,
                          cfg.chunk_delay_ms, tag);
        if (rc != REQ_OK) return rc;
        Logger::info(tag, "sent " + std::to_string(cfg.count) + " pipelined request(s)");
        for (int i = 0; i < cfg.count; ++i) {
            rc = recv_one(i);
            if (rc != REQ_OK) return rc;
        }
    } else {
        for (int i = 0; i < cfg.count; ++i) {
            int rc = send_one(i);
            if (rc != REQ_OK) return rc;
            rc = recv_one(i);
            if (rc != REQ_OK) return rc;
        }
    }
    Logger::info(tag, "all " + std::to_string(cfg.count) +
                 " response(s) received in request order");
    return REQ_OK;
}

// hold 模式: 连接后保持沉默，观察服务端是否因空闲超时关闭连接
int hold_mode(int fd, const RequestConfig& cfg, const std::string& tag) {
    Logger::info(tag, "holding connection " + std::to_string(cfg.hold_sec) +
                 "s without sending");
    auto start = Clock::now();
    auto deadline = start + std::chrono::seconds(cfg.hold_sec);
    while (Clock::now() < deadline) {
        pollfd pfd {fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, 100);
        if (r < 0) {
            if (errno == EINTR) continue;
            Logger::error(tag, std::string("poll 失败: ") + strerror(errno));
            return REQ_ERR_IO;
        }
        if (r == 0) continue;
        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            char tmp[256];
            ssize_t n = ::read(fd, tmp, sizeof(tmp));
            if (n <= 0) {
                long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - start).count();
                Logger::error(tag, "connection closed by server after " +
                              std::to_string(elapsed) + "ms");
                return REQ_ERR_CLOSED_BY_SERVER;
            }
            Logger::warn(tag, "unexpected data while holding (" +
                         std::to_string(n) + " bytes)");
        }
    }
    Logger::info(tag, "hold complete, server kept connection open");
    return REQ_OK;
}

// jumbo 模式: 只发送声明超大长度的帧头，观察服务端拒绝并关闭连接
int jumbo_mode(int fd, const RequestConfig& cfg, const std::string& tag) {
    uint8_t hdr[proto::HEADER_SIZE];
    proto::encode_header(hdr, proto::TYPE_REQUEST, 1, cfg.jumbo_len);
    int rc = send_all(fd, hdr, sizeof(hdr), 0, 0, tag);
    if (rc != REQ_OK) return rc;
    Logger::info(tag, "sent frame header declaring payload_len=" +
                 std::to_string(cfg.jumbo_len) + " (no payload follows)");
    uint32_t rid = 0;
    std::string payload;
    rc = recv_frame(fd, rid, payload, cfg.recv_timeout_ms, cfg, tag);
    if (rc == REQ_OK) {
        Logger::error(tag, "server did not reject oversize frame");
        return REQ_ERR_MISMATCH;
    }
    return rc;  // 期望 REQ_ERR_CLOSED_BY_SERVER
}

void print_request_usage() {
    std::cout <<
        "用法: ipc_demo request [选项]\n"
        "  --path PATH          服务端 Socket 路径 (默认 /tmp/ipc_proxy.sock)\n"
        "  --count N            请求数量 (默认 4)\n"
        "  --message TEXT       请求内容 (默认 \"hello\")\n"
        "  --payload-size N     生成 N 字节的请求内容 (覆盖 --message)\n"
        "  --sleep-ms MS        首个请求要求服务端延迟 MS 毫秒响应\n"
        "  --pipeline           先连续发送全部请求再统一读响应 (粘包)\n"
        "  --chunk-bytes N      每次只写 N 字节 (拆包, 0=一次写完)\n"
        "  --chunk-delay-ms MS  写块之间的间隔\n"
        "  --read-chunk N       每次只读 N 字节 (慢消费者)\n"
        "  --read-delay-ms MS   读块之间的间隔\n"
        "  --hold SEC           连接后保持 SEC 秒不发送任何数据\n"
        "  --jumbo LEN          只发送声明 LEN 字节的帧头 (触发超限拒绝)\n"
        "  --recv-timeout MS    单个响应的接收超时 (默认 10000)\n"
        "  --client-id NAME     日志中的客户端标识\n"
        "  --help               显示本说明\n"
        "退出码: 0=成功 2=连接失败 3=I/O错误 4=被服务端关闭 5=响应校验失败 6=超时\n";
}

} // namespace

int run_socket_request(const RequestConfig& cfg) {
    std::string tag = cfg.client_id.empty() ? "REQUEST" : ("REQUEST-" + cfg.client_id);

    // 写已关闭的连接时拿 EPIPE 而不是进程被杀
    struct sigaction ign {};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    ::sigaction(SIGPIPE, &ign, nullptr);

    if (cfg.path.size() >= sizeof(((struct sockaddr_un*)nullptr)->sun_path)) {
        Logger::error(tag, "socket 路径过长: " + cfg.path);
        return REQ_ERR_USAGE;
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        Logger::error(tag, std::string("创建 socket 失败: ") + strerror(errno));
        return REQ_ERR_IO;
    }
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, cfg.path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        Logger::error(tag, "connect 失败 " + cfg.path + ": " + strerror(errno));
        ::close(fd);
        return REQ_ERR_CONNECT;
    }
    Logger::info(tag, "connected to " + cfg.path);

    int rc;
    if (cfg.hold_sec >= 0) {
        rc = hold_mode(fd, cfg, tag);
    } else if (cfg.jumbo_len > 0) {
        rc = jumbo_mode(fd, cfg, tag);
    } else {
        rc = normal_mode(fd, cfg, tag);
    }
    ::close(fd);
    return rc;
}

int socket_request_main(int argc, char** argv) {
    RequestConfig cfg;
    try {
        for (int i = 0; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&](const char* name) -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(std::string("缺少参数值: ") + name);
                return argv[++i];
            };
            if (a == "--path") {
                cfg.path = value("--path");
            } else if (a == "--count") {
                cfg.count = std::stoi(value("--count"));
            } else if (a == "--message") {
                cfg.message = value("--message");
            } else if (a == "--payload-size") {
                cfg.payload_size = std::stoi(value("--payload-size"));
            } else if (a == "--sleep-ms") {
                cfg.sleep_ms = std::stoi(value("--sleep-ms"));
            } else if (a == "--pipeline") {
                cfg.pipeline = true;
            } else if (a == "--chunk-bytes") {
                cfg.chunk_bytes = std::stoi(value("--chunk-bytes"));
            } else if (a == "--chunk-delay-ms") {
                cfg.chunk_delay_ms = std::stoi(value("--chunk-delay-ms"));
            } else if (a == "--read-chunk") {
                cfg.read_chunk_bytes = std::stoi(value("--read-chunk"));
            } else if (a == "--read-delay-ms") {
                cfg.read_delay_ms = std::stoi(value("--read-delay-ms"));
            } else if (a == "--hold") {
                cfg.hold_sec = std::stoi(value("--hold"));
            } else if (a == "--jumbo") {
                cfg.jumbo_len = static_cast<uint32_t>(std::stoul(value("--jumbo")));
            } else if (a == "--recv-timeout") {
                cfg.recv_timeout_ms = std::stoi(value("--recv-timeout"));
            } else if (a == "--client-id") {
                cfg.client_id = value("--client-id");
            } else if (a == "--help" || a == "-h") {
                print_request_usage();
                return REQ_OK;
            } else {
                std::cerr << "未知参数: " << a << std::endl;
                print_request_usage();
                return REQ_ERR_USAGE;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "参数解析失败: " << e.what() << std::endl;
        print_request_usage();
        return REQ_ERR_USAGE;
    }
    if (cfg.count <= 0 && cfg.hold_sec < 0 && cfg.jumbo_len == 0) {
        std::cerr << "--count 必须大于 0" << std::endl;
        return REQ_ERR_USAGE;
    }
    return run_socket_request(cfg);
}

} // namespace ipc
