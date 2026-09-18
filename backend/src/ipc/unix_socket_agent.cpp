/**
 * unix_socket_agent.cpp
 *
 * serve / request 两种工作模式的实现。
 *
 * serve：
 * - 主线程 poll 监听套接字与 signalfd，只负责 accept 与关停
 * - 每条连接一个独立工作线程：顺序读取请求 -> 处理 -> 写回响应，
 *   单连接内严格按请求顺序响应；某条连接慢（睡眠 / 网络抖动）只会阻塞
 *   它自己的线程，不影响其它连接
 * - SIGTERM/SIGINT：停止 accept，shutdown(rd) 唤醒读阻塞的线程，
 *   已在处理中的请求允许在宽限期内完成并写回；超时则强制关闭并返回退出码 2
 *
 * request：
 * - 按顺序发送若干带 request_id 的帧（可逐字节发送模拟半包），
 *   再按顺序读取响应并校验 request_id，结果以 "RESPONSE <id> <payload>"
 *   行输出到 stdout，日志输出到 stderr
 */

#include "unix_socket_agent.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <signal.h>
#include <sys/signalfd.h>
#include <thread>

namespace ipc {
namespace uxsock {

namespace {

// ---- 日志（带时间戳，事件写到 stderr，机器可 grep）--------------------------

std::string now_stamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03d", buf,
                  static_cast<int>(ms.count()));
    return out;
}

#define LOG_SERVER(fmt, ...)                                                  \
    do {                                                                      \
        std::fprintf(stderr, "[%s] [uxsock-server] " fmt "\n",               \
                     now_stamp().c_str(), ##__VA_ARGS__);                     \
    } while (0)

#define LOG_CLIENT(fmt, ...)                                                  \
    do {                                                                      \
        std::fprintf(stderr, "[%s] [uxsock-client] " fmt "\n",               \
                     now_stamp().c_str(), ##__VA_ARGS__);                     \
    } while (0)

// ---- 请求处理（演示用业务逻辑）---------------------------------------------
//
// 普通请求：响应 "ACK:<payload>"
// SLEEP:<毫秒>:<内容>：睡眠指定毫秒后响应 "ACK:<内容>"，
// 用来模拟慢插件并验证“慢连接不阻塞其它连接”。
// 无法解析的毫秒数通过 ERROR 帧返回（应用层错误，不关闭连接）。
Frame handle_request(const Frame& req, uint32_t max_frame_bytes) {
    Frame resp;
    resp.request_id = req.request_id;
    resp.type = FRAME_RESPONSE;

    static const std::string kPrefix = "SLEEP:";
    if (req.payload.compare(0, kPrefix.size(), kPrefix) != 0) {
        resp.payload = "ACK:" + req.payload;
        return resp;
    }

    const std::string& s = req.payload;
    size_t colon = s.find(':', kPrefix.size());
    if (colon == std::string::npos) {
        resp.type = FRAME_ERROR;
        resp.payload = "bad-request: expected SLEEP:<ms>:<body>";
        return resp;
    }
    std::string num = s.substr(kPrefix.size(), colon - kPrefix.size());
    char* end = nullptr;
    errno = 0;
    long ms = std::strtol(num.c_str(), &end, 10);
    if (errno != 0 || end != num.c_str() + num.size() || ms < 0) {
        resp.type = FRAME_ERROR;
        resp.payload = "bad-request: invalid sleep milliseconds";
        return resp;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    resp.payload = "ACK:" + s.substr(colon + 1);
    (void)max_frame_bytes;
    return resp;
}

// ---- 服务端共享状态 ---------------------------------------------------------

struct ConnState {
    int fd;
    uint64_t id;
};

struct ServerState {
    explicit ServerState(ServerConfig c) : cfg(std::move(c)) {}

    ServerConfig cfg;
    std::atomic<bool> shutdown{false};

    std::mutex mu;
    std::condition_variable cv;
    std::deque<ConnState> conns; // 当前存活连接
    std::vector<std::thread> workers; // 工作线程句柄
    uint64_t next_conn_id = 1;

    uint64_t add_conn(int fd, size_t* total_out) {
        std::lock_guard<std::mutex> lk(mu);
        uint64_t id = next_conn_id++;
        conns.push_back({fd, id});
        if (total_out) *total_out = conns.size();
        return id;
    }

    void add_worker(std::thread t) {
        std::lock_guard<std::mutex> lk(mu);
        workers.push_back(std::move(t));
    }

    void remove_conn(int fd) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto it = conns.begin(); it != conns.end(); ++it) {
            if (it->fd == fd) {
                conns.erase(it);
                break;
            }
        }
        cv.notify_all();
    }

    std::vector<int> all_fds() {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<int> fds;
        fds.reserve(conns.size());
        for (const auto& c : conns) fds.push_back(c.fd);
        return fds;
    }

    // 等待所有连接结束；返回 true 表示在宽限期内全部结束。
    bool wait_all_done(int grace_ms) {
        std::unique_lock<std::mutex> lk(mu);
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(grace_ms);
        return cv.wait_until(lk, deadline,
                             [this] { return conns.empty(); });
    }
};

// 单条连接的工作线程
void serve_connection(std::shared_ptr<ServerState> state, int fd,
                      uint64_t conn_id) {
    FrameReader reader;
    uint64_t requests = 0;
    const char* reason = nullptr;

    auto close_reason = [&](ProtocolError pe) -> const char* {
        if (state->shutdown.load()) return "server-shutdown";
        switch (pe) {
            case ProtocolError::PeerClosed:
                // 缓冲区里还有半个帧就 EOF，属于客户端中途异常断开
                return reader.buffered_bytes() > 0 ? "client-aborted"
                                                   : "peer-closed";
            case ProtocolError::IdleTimeout: return "idle-timeout";
            case ProtocolError::LengthTooLarge: return "frame-too-large";
            case ProtocolError::LengthTooSmall:
                return "frame-length-invalid";
            case ProtocolError::BadFlags: return "bad-flags";
            case ProtocolError::BadReserved: return "bad-reserved";
            case ProtocolError::BadType: return "bad-type";
            default: return "io-error";
        }
    };

    while (!state->shutdown.load()) {
        Frame req;
        ProtocolError pe = reader.read_frame(
            fd, state->cfg.max_frame_bytes, &req,
            state->cfg.idle_timeout_ms > 0
                ? state->cfg.idle_timeout_ms
                : -1);
        if (pe != ProtocolError::None) {
            reason = close_reason(pe);
            break;
        }
        if (req.type != FRAME_REQUEST) {
            reason = "unexpected-frame-type";
            break;
        }

        ++requests;
        LOG_SERVER("conn#%llu request id=%llu bytes=%zu",
                   static_cast<unsigned long long>(conn_id),
                   static_cast<unsigned long long>(req.request_id),
                   req.payload.size());

        // 处理可能较慢（SLEEP），仅阻塞当前连接的线程
        Frame resp = handle_request(req, state->cfg.max_frame_bytes);

        // 关停信号在处理期间到达：这是“已接收请求”，仍把结果写完
        if (!write_frame(fd, resp, state->cfg.max_frame_bytes)) {
            reason = state->shutdown.load() ? "server-shutdown"
                                            : "write-io-error";
            break;
        }
        LOG_SERVER("conn#%llu response id=%llu type=%s bytes=%zu",
                   static_cast<unsigned long long>(conn_id),
                   static_cast<unsigned long long>(resp.request_id),
                   resp.type == FRAME_ERROR ? "error" : "response",
                   resp.payload.size());

        if (state->shutdown.load()) {
            // 在途响应已写出，随优雅关停结束
            reason = "server-shutdown";
        }
    }

    if (!reason) reason = "peer-closed";

    ::close(fd);
    state->remove_conn(fd);
    LOG_SERVER("conn#%llu closed reason=%s requests=%llu",
               static_cast<unsigned long long>(conn_id), reason,
               static_cast<unsigned long long>(requests));
}

// 阻塞 SIGTERM/SIGINT，返回 signalfd
int make_signalfd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);

    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0) {
        std::fprintf(stderr, "[uxsock-server] signalfd: %s\n",
                     std::strerror(errno));
    }
    return sfd;
}

} // namespace

// ---- serve 模式 -------------------------------------------------------------

int run_serve(const ServerConfig& cfg_in) {
    ServerConfig cfg = cfg_in;

    // SIGPIPE 全局忽略（写已关闭套接字时返回 EPIPE 而不是杀进程）
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, nullptr);

    int sfd = make_signalfd();
    if (sfd < 0) return 1;

    std::string err;
    int listen_fd = create_listening_socket(cfg, &err);
    if (listen_fd < 0) {
        LOG_SERVER("startup failed: %s", err.c_str());
        return 1;
    }

    LOG_SERVER("listening path=%s max_frame=%u idle_timeout_ms=%d grace_ms=%d",
               cfg.path.c_str(), cfg.max_frame_bytes, cfg.idle_timeout_ms,
               cfg.grace_ms);

    auto state = std::make_shared<ServerState>(cfg);

    auto begin_shutdown = [&](const char* sig_name) {
        if (state->shutdown.exchange(true)) return;
        size_t active = 0;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            active = state->conns.size();
        }
        LOG_SERVER("signal %s received: stop accepting, active=%zu grace_ms=%d",
                   sig_name, active, cfg.grace_ms);

        // 停止接收新连接；Socket 文件等宽限期结束后再清理
        ::close(listen_fd);

        // 唤醒阻塞在读上的工作线程（只关读半区，保证在途响应仍可写出）
        for (int cfd : state->all_fds()) {
            ::shutdown(cfd, SHUT_RD);
        }
    };

    // 主事件循环：listen_fd 与 signalfd
    for (;;) {
        struct pollfd pfds[2];
        pfds[0].fd = listen_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = sfd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int rc = ::poll(pfds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOG_SERVER("poll: %s", std::strerror(errno));
            state->shutdown.store(true);
            ::close(listen_fd);
            ::unlink(cfg.path.c_str());
            return 1;
        }

        if (pfds[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            ssize_t n = ::read(sfd, &si, sizeof(si));
            (void)n;
            begin_shutdown(si.ssi_signo == SIGTERM ? "SIGTERM" : "SIGINT");
            break;
        }

        if (pfds[0].revents & POLLIN) {
            int cfd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (cfd < 0) {
                if (errno == EINTR) continue;
                LOG_SERVER("accept: %s", std::strerror(errno));
                continue;
            }
            size_t total = 0;
            uint64_t id = state->add_conn(cfd, &total);
            LOG_SERVER("conn#%llu opened fd=%d total=%zu",
                       static_cast<unsigned long long>(id), cfd, total);
            state->add_worker(
                std::thread(serve_connection, state, cfd, id));
        }
    }

    // 优雅关停：等待在途请求处理完
    bool clean = state->wait_all_done(cfg.grace_ms);

    if (clean) {
        LOG_SERVER("all connections finished within grace period");
        // 所有连接线程均已结束，回收线程句柄后再清理节点
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            workers.swap(state->workers);
        }
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    } else {
        std::vector<int> stragglers = state->all_fds();
        LOG_SERVER("grace period (%d ms) expired, %zu connection(s) still "
                   "active: force closing; exiting with failure",
                   cfg.grace_ms, stragglers.size());
        // 只 shutdown 唤醒阻塞的工作线程，fd 仍归线程关闭；
        // Socket 节点必须先清理，再以非 0 退出码明确失败。
        for (int cfd : stragglers) {
            ::shutdown(cfd, SHUT_RDWR);
        }
        if (::unlink(cfg.path.c_str()) != 0 && errno != ENOENT) {
            LOG_SERVER("warning: unlink %s: %s", cfg.path.c_str(),
                       std::strerror(errno));
        } else {
            LOG_SERVER("socket node removed: %s", cfg.path.c_str());
        }
        LOG_SERVER("stopped exit=grace-timeout");
        std::fflush(stderr);
        // 分离线程仍持有 fd，直接 _exit 让内核回收，避免重复 close
        _exit(2);
    }

    if (::unlink(cfg.path.c_str()) == 0) {
        LOG_SERVER("socket node removed: %s", cfg.path.c_str());
    } else if (errno != ENOENT) {
        LOG_SERVER("warning: unlink %s: %s", cfg.path.c_str(),
                   std::strerror(errno));
    }

    LOG_SERVER("stopped exit=ok");
    return 0;
}

// ---- request 模式 -----------------------------------------------------------
//
// 客户端退出码：
//  0 全部请求收到顺序正确的响应
//  1 参数错误
//  2 连接服务端失败
//  3 服务端在收齐响应前关闭连接（如帧被拒绝）
//  4 收到超过本端上限的帧
//  5 协议错误（字段非法 / request_id 乱序 / 帧类型错误）
//  6 IO 错误或超时

namespace {

// 按可选项发送一帧（或原始字节）
bool send_with_options(int fd, const std::vector<uint8_t>& bytes,
                       bool byte_at_a_time, int delay_us) {
    if (!byte_at_a_time) {
        return write_full(fd, bytes.data(), bytes.size());
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
        uint8_t b = bytes[i];
        if (!write_full(fd, &b, 1)) return false;
        if (delay_us > 0) {
            ::usleep(static_cast<useconds_t>(delay_us));
        }
    }
    return true;
}

bool parse_hex(const std::string& hex, std::vector<uint8_t>* out) {
    if (hex.size() % 2 != 0) return false;
    out->clear();
    out->reserve(hex.size() / 2);
    auto nib = [](char c, int* v) {
        if (c >= '0' && c <= '9') *v = c - '0';
        else if (c >= 'a' && c <= 'f') *v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') *v = c - 'A' + 10;
        else return false;
        return true;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi, lo;
        if (!nib(hex[i], &hi) || !nib(hex[i + 1], &lo)) return false;
        out->push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

} // namespace

int run_request(const RequestOptions& opts) {
    std::string err;
    int fd = connect_to_server(opts.path, opts.connect_timeout_ms, &err);
    if (fd < 0) {
        LOG_CLIENT("%s", err.c_str());
        return 2;
    }
    LOG_CLIENT("connected path=%s requests=%zu byte_at_a_time=%d",
               opts.path.c_str(), opts.payloads.size(),
               opts.byte_at_a_time ? 1 : 0);

    // 发送阶段：逐帧顺序发送，可逐字节制造半包
    uint64_t next_id = 1;
    size_t expected_responses = 0;

    if (!opts.raw_blobs.empty()) {
        // 协议探针模式：原样发送用户构造的字节（用于非法字段测试）
        for (const auto& blob : opts.raw_blobs) {
            std::vector<uint8_t> bytes;
            if (!parse_hex(blob, &bytes)) {
                LOG_CLIENT("invalid --send-raw-hex value");
                ::close(fd);
                return 1;
            }
            if (!send_with_options(fd, bytes, opts.byte_at_a_time,
                                   opts.delay_us)) {
                LOG_CLIENT("send failed: %s", std::strerror(errno));
                ::close(fd);
                return 6;
            }
            ++expected_responses;
        }
    } else {
        for (const auto& payload : opts.payloads) {
            Frame req;
            req.request_id = next_id++;
            req.type = FRAME_REQUEST;
            req.payload = payload;
            std::vector<uint8_t> bytes;
            if (!encode_frame(req, kMaxFrameHardLimit, &bytes)) {
                LOG_CLIENT("frame too large to encode (hard limit %u bytes)",
                           kMaxFrameHardLimit);
                ::close(fd);
                return 1;
            }
            if (!send_with_options(fd, bytes, opts.byte_at_a_time,
                                   opts.delay_us)) {
                LOG_CLIENT("send failed: %s", std::strerror(errno));
                ::close(fd);
                return 6;
            }
            ++expected_responses;
        }
    }

    // 接收阶段：按请求顺序校验 request_id
    FrameReader reader;
    uint64_t expected_id = 1;
    size_t got = 0;
    while (got < expected_responses) {
        Frame resp;
        ProtocolError pe =
            reader.read_frame(fd, opts.max_frame_bytes, &resp, -1);
        if (pe == ProtocolError::PeerClosed) {
            LOG_CLIENT("server closed connection after %zu/%zu response(s)",
                       got, expected_responses);
            ::close(fd);
            return 3;
        }
        if (pe == ProtocolError::LengthTooLarge) {
            LOG_CLIENT("received frame exceeds local max_frame=%u",
                       opts.max_frame_bytes);
            ::close(fd);
            return 4;
        }
        if (pe != ProtocolError::None) {
            LOG_CLIENT("read failed: %s", protocol_error_name(pe));
            ::close(fd);
            return 6;
        }
        if (resp.type != FRAME_RESPONSE && resp.type != FRAME_ERROR) {
            LOG_CLIENT("unexpected frame type %u",
                       static_cast<unsigned>(resp.type));
            ::close(fd);
            return 5;
        }
        if (resp.request_id != expected_id) {
            LOG_CLIENT("response out of order: got id=%llu expected=%llu",
                       static_cast<unsigned long long>(resp.request_id),
                       static_cast<unsigned long long>(expected_id));
            ::close(fd);
            return 5;
        }
        ++expected_id;
        ++got;

        // stdout 仅输出机器可解析的结果行
        std::printf("%s %llu %s\n",
                    resp.type == FRAME_ERROR ? "ERROR" : "RESPONSE",
                    static_cast<unsigned long long>(resp.request_id),
                    resp.payload.c_str());
        std::fflush(stdout);
    }

    // 主动半关闭写端，告诉服务端请求已全部发完
    ::shutdown(fd, SHUT_WR);
    ::close(fd);
    LOG_CLIENT("done, %zu response(s) received in order", got);
    return 0;
}

} // namespace uxsock
} // namespace ipc
