/**
 * Unix Domain Socket 本地代理 - 服务端 (serve 模式)
 *
 * 特性：
 * - 单线程 poll 事件循环，可同时维护多个客户端连接
 * - 固定 12 字节头 (大端序) + request_id 的帧协议，正确处理拆包/粘包
 * - 每连接独立读缓冲与发送缓冲，短写保留剩余部分等下次 POLLOUT，
 *   慢消费者不会阻塞其他连接
 * - 同一连接内响应严格按请求顺序发出 (前序请求未完成时后续响应排队)
 * - 帧长超限 / 空闲超时 / 协议字段非法时关闭对应连接，并记录可区分的原因
 * - 路径被占用时仅移除确认失效 (connect 两次 ECONNREFUSED) 的 Socket 节点
 * - 创建后 chmod 0600，仅当前用户可读写
 * - SIGTERM 后停止接收新连接，宽限期内完成在途请求再清理 Socket 文件，
 *   超期则以退出码 SERVE_ERR_GRACE_EXPIRED 明确失败
 */

#include "socket_proxy.h"
#include "socket_proto.h"
#include "ipc_demo.h"

#include <poll.h>

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace ipc {
namespace {

using Clock = std::chrono::steady_clock;
constexpr const char* TAG = "SERVE";

// ---------------------------------------------------------------------------
// SIGTERM 处理: 仅置标志并写自管道，事件循环被唤醒后统一处理
// ---------------------------------------------------------------------------
volatile sig_atomic_t g_got_term = 0;
int g_self_pipe[2] = {-1, -1};

void on_sigterm(int) {
    g_got_term = 1;
    if (g_self_pipe[1] >= 0) {
        const char b = 'T';
        ssize_t n = ::write(g_self_pipe[1], &b, 1);  // 非阻塞，写不进也无所谓
        (void)n;
    }
}

bool install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = on_sigterm;
    sigemptyset(&sa.sa_mask);
    if (::sigaction(SIGTERM, &sa, nullptr) != 0) return false;
    if (::sigaction(SIGINT, &sa, nullptr) != 0) return false;

    // 写已断开的连接时拿到 EPIPE 而不是进程被杀
    struct sigaction ign {};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    return ::sigaction(SIGPIPE, &ign, nullptr) == 0;
}

bool set_non_blocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void set_cloexec(int fd) {
    int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// ---------------------------------------------------------------------------
// 连接关闭原因 (日志中以 reason=<token> 形式输出，便于区分与检索)
// ---------------------------------------------------------------------------
enum class CloseReason {
    ClientDisconnect,    // 客户端中途断开 (含半包残留)
    IdleTimeout,         // 空闲超时
    FrameTooLarge,       // 帧长超过配置上限
    ProtocolError,       // 协议字段非法 (magic/version/type)
    ServerShutdown,      // 服务端退出前清理
    SendBufferOverflow,  // 慢消费者导致发送缓冲超限
    IoError,             // 读写系统调用错误
};

const char* reason_token(CloseReason r) {
    switch (r) {
        case CloseReason::ClientDisconnect:   return "client-disconnect";
        case CloseReason::IdleTimeout:        return "idle-timeout";
        case CloseReason::FrameTooLarge:      return "frame-too-large";
        case CloseReason::ProtocolError:      return "protocol-error";
        case CloseReason::ServerShutdown:     return "server-shutdown";
        case CloseReason::SendBufferOverflow: return "send-buffer-overflow";
        case CloseReason::IoError:            return "io-error";
    }
    return "unknown";
}

// 待发送响应: 同一连接内按请求顺序排队，前序未就绪时后续不能先发
struct PendingResp {
    uint32_t request_id = 0;
    bool ready = false;               // 响应内容已就绪
    bool timed = false;               // 是否定时请求 (SLEEP)
    Clock::time_point deadline;       // 定时请求的到期时间
    std::string payload;              // 响应内容
};

struct ClientConn {
    int fd = -1;
    uint64_t cid = 0;                       // 连接编号 (日志用)
    std::vector<uint8_t> inbuf;             // 读缓冲 (处理拆包/粘包)
    std::size_t in_off = 0;
    std::deque<PendingResp> pending;        // 按请求顺序的响应队列
    std::vector<uint8_t> outbuf;            // 发送缓冲 (处理短写)
    std::size_t out_off = 0;
    Clock::time_point last_activity;        // 最近一次读写时间 (空闲判定)
    bool closed = false;
};

// 解析 "SLEEP:<ms>:<text>" 请求；不是该格式时返回 false
bool parse_sleep_request(const std::string& payload, long& ms_out, std::string& text_out) {
    static const std::string prefix = "SLEEP:";
    if (payload.compare(0, prefix.size(), prefix) != 0) return false;
    std::string rest = payload.substr(prefix.size());
    std::size_t pos = rest.find(':');
    if (pos == std::string::npos) return false;
    try {
        long ms = std::stol(rest.substr(0, pos));
        if (ms < 0) return false;
        ms_out = std::min<long>(ms, 60000);  // 上限 60s，防止异常定时
        text_out = rest.substr(pos + 1);
        return true;
    } catch (...) {
        return false;
    }
}

class ProxyServer {
public:
    explicit ProxyServer(const ServeConfig& cfg) : cfg_(cfg) {}

    int run();

private:
    int prepare_path();
    void accept_all();
    void do_read(ClientConn* c);
    void do_write(ClientConn* c);
    void parse_frames(ClientConn* c);
    void dispatch(ClientConn* c, uint32_t request_id, const std::string& payload);
    void promote(ClientConn* c);
    void process_timers();
    void check_idle();
    void begin_shutdown();
    void close_client(ClientConn* c, CloseReason reason, const std::string& detail);
    void reap_closed();
    int compute_timeout_ms() const;
    std::size_t count_inflight() const;

    ServeConfig cfg_;
    int listen_fd_ = -1;
    bool bound_ = false;             // 是否已 bind (退出时据此清理 Socket 文件)
    bool shutting_down_ = false;
    Clock::time_point grace_deadline_;
    Clock::time_point accept_cooldown_until_{};
    std::map<int, std::unique_ptr<ClientConn>> clients_;
    uint64_t conn_seq_ = 0;
    uint64_t total_requests_ = 0;
    uint64_t total_responses_ = 0;
    int exit_code_ = SERVE_OK;
};

// 路径占用检查: 仅当确认是失效 Socket (connect 两次均 ECONNREFUSED) 时才移除
int ProxyServer::prepare_path() {
    struct stat st {};
    if (::lstat(cfg_.path.c_str(), &st) != 0) {
        if (errno == ENOENT) return SERVE_OK;  // 路径空闲
        Logger::error(TAG, "无法检查路径 " + cfg_.path + ": " + strerror(errno));
        return SERVE_ERR_GENERIC;
    }
    if (!S_ISSOCK(st.st_mode)) {
        Logger::error(TAG, "path exists and is not a socket, refuse to remove: " + cfg_.path);
        return SERVE_ERR_PATH_BUSY;
    }
    // 是 Socket 节点: 探测是否有活跃服务端。连续两次 ECONNREFUSED 才确认失效，
    // 避免误删 listen 积压恰好已满的正常服务。
    for (int attempt = 0; attempt < 2; ++attempt) {
        int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe < 0) {
            Logger::error(TAG, std::string("创建探测 socket 失败: ") + strerror(errno));
            return SERVE_ERR_GENERIC;
        }
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, cfg_.path.c_str(), sizeof(addr.sun_path) - 1);
        int rc = ::connect(probe, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        int e = errno;
        ::close(probe);
        if (rc == 0) {
            Logger::error(TAG, "path already in use by a live server: " + cfg_.path);
            return SERVE_ERR_PATH_BUSY;
        }
        if (e != ECONNREFUSED) {
            Logger::error(TAG, "socket exists but probe failed (" + std::string(strerror(e)) +
                          "), refuse to remove: " + cfg_.path);
            return SERVE_ERR_PATH_BUSY;
        }
        if (attempt == 0) usleep(50000);  // 50ms 后再次确认
    }
    Logger::warn(TAG, "removing stale socket: " + cfg_.path);
    if (::unlink(cfg_.path.c_str()) != 0) {
        Logger::error(TAG, "删除失效 socket 失败: " + std::string(strerror(errno)));
        return SERVE_ERR_GENERIC;
    }
    return SERVE_OK;
}

void ProxyServer::accept_all() {
    while (true) {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EMFILE || errno == ENFILE) {
                // fd 耗尽: 稍作冷却，避免 listen 可读导致忙轮询
                Logger::warn(TAG, std::string("accept 暂时失败: ") + strerror(errno));
                accept_cooldown_until_ = Clock::now() + std::chrono::milliseconds(200);
                return;
            }
            Logger::error(TAG, std::string("accept 失败: ") + strerror(errno));
            return;
        }
        set_cloexec(fd);
        set_non_blocking(fd);
        auto c = std::make_unique<ClientConn>();
        c->fd = fd;
        c->cid = ++conn_seq_;
        c->last_activity = Clock::now();
        clients_[fd] = std::move(c);
        Logger::info(TAG, "conn#" + std::to_string(conn_seq_) + " accepted (active=" +
                     std::to_string(clients_.size()) + ")");
    }
}

void ProxyServer::do_read(ClientConn* c) {
    uint8_t buf[16384];
    while (!c->closed) {
        ssize_t n = ::read(c->fd, buf, sizeof(buf));
        if (n > 0) {
            c->inbuf.insert(c->inbuf.end(), buf, buf + n);
            c->last_activity = Clock::now();
            parse_frames(c);  // 每次读入后立即解析，读缓冲始终有界
        } else if (n == 0) {
            // 客户端中途断开: 可能还有未收完的半包与未完成的请求
            std::string detail = "peer closed";
            std::size_t leftover = c->inbuf.size() - c->in_off;
            if (leftover > 0) {
                detail += " with " + std::to_string(leftover) +
                          " byte(s) of incomplete frame discarded";
            }
            close_client(c, CloseReason::ClientDisconnect, detail);
            return;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            close_client(c, CloseReason::IoError, std::string("read: ") + strerror(errno));
            return;
        }
    }
}

void ProxyServer::parse_frames(ClientConn* c) {
    while (!c->closed) {
        std::size_t avail = c->inbuf.size() - c->in_off;
        if (avail < proto::HEADER_SIZE) return;  // 半包: 头部未收齐，等待后续数据

        const uint8_t* h = c->inbuf.data() + c->in_off;
        if (h[0] != proto::MAGIC0 || h[1] != proto::MAGIC1) {
            close_client(c, CloseReason::ProtocolError, "bad magic");
            return;
        }
        if (h[2] != proto::VERSION) {
            close_client(c, CloseReason::ProtocolError,
                         "unsupported version " + std::to_string(h[2]));
            return;
        }
        if (h[3] != proto::TYPE_REQUEST) {
            close_client(c, CloseReason::ProtocolError,
                         "unexpected frame type " + std::to_string(h[3]));
            return;
        }
        uint32_t request_id = proto::get_u32be(h + 4);
        uint32_t len = proto::get_u32be(h + 8);
        if (len > cfg_.max_frame) {
            close_client(c, CloseReason::FrameTooLarge,
                         "declared=" + std::to_string(len) +
                         " max=" + std::to_string(cfg_.max_frame));
            return;
        }
        if (avail < proto::HEADER_SIZE + len) return;  // 半包: 正文未收齐

        std::string payload(reinterpret_cast<const char*>(h) + proto::HEADER_SIZE, len);
        c->in_off += proto::HEADER_SIZE + len;
        if (c->in_off == c->inbuf.size()) {  // 全部消费完则复位
            c->inbuf.clear();
            c->in_off = 0;
        }
        dispatch(c, request_id, payload);  // 粘包: 循环继续解析下一帧
    }
}

void ProxyServer::dispatch(ClientConn* c, uint32_t request_id, const std::string& payload) {
    ++total_requests_;
    PendingResp p;
    p.request_id = request_id;
    long ms = 0;
    std::string text;
    if (parse_sleep_request(payload, ms, text)) {
        // 慢请求: 到期前不就绪，但不阻塞其他连接的处理
        p.timed = true;
        p.deadline = Clock::now() + std::chrono::milliseconds(ms);
        p.payload = "OK:" + text;
        c->pending.push_back(std::move(p));
        Logger::info(TAG, "conn#" + std::to_string(c->cid) + " request id=" +
                     std::to_string(request_id) + " len=" + std::to_string(payload.size()) +
                     " deferred " + std::to_string(ms) + "ms");
    } else {
        p.ready = true;
        p.payload = "ECHO:" + payload;
        c->pending.push_back(std::move(p));
        Logger::info(TAG, "conn#" + std::to_string(c->cid) + " request id=" +
                     std::to_string(request_id) + " len=" + std::to_string(payload.size()));
    }
    promote(c);
}

// 将队首已就绪的响应依次移入发送缓冲，保证单连接内按请求顺序返回
void ProxyServer::promote(ClientConn* c) {
    while (!c->pending.empty() && c->pending.front().ready) {
        PendingResp& p = c->pending.front();
        std::vector<uint8_t> frame = proto::build_frame(proto::TYPE_RESPONSE, p.request_id, p.payload);
        c->outbuf.insert(c->outbuf.end(), frame.begin(), frame.end());
        ++total_responses_;
        Logger::info(TAG, "conn#" + std::to_string(c->cid) + " response id=" +
                     std::to_string(p.request_id) + " queued (len=" +
                     std::to_string(p.payload.size()) + ")");
        c->pending.pop_front();
    }
    if (c->outbuf.size() - c->out_off > cfg_.max_out_buf) {
        close_client(c, CloseReason::SendBufferOverflow,
                     "outbuf exceeds " + std::to_string(cfg_.max_out_buf));
    }
}

void ProxyServer::do_write(ClientConn* c) {
    while (c->out_off < c->outbuf.size()) {
        ssize_t n = ::write(c->fd, c->outbuf.data() + c->out_off, c->outbuf.size() - c->out_off);
        if (n > 0) {
            c->out_off += static_cast<std::size_t>(n);  // 短写: 保留剩余部分
            c->last_activity = Clock::now();
        } else if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // 等下次 POLLOUT
            close_client(c, CloseReason::IoError, std::string("write: ") + strerror(errno));
            return;
        } else {
            return;  // write 返回 0: 视为暂时不可写
        }
    }
    if (c->out_off == c->outbuf.size()) {
        c->outbuf.clear();
        c->out_off = 0;
    }
}

void ProxyServer::process_timers() {
    auto now = Clock::now();
    for (auto& kv : clients_) {
        ClientConn* c = kv.second.get();
        if (c->closed) continue;
        bool changed = false;
        for (auto& p : c->pending) {
            if (p.timed && !p.ready && p.deadline <= now) {
                p.ready = true;
                changed = true;
            }
        }
        if (changed) promote(c);
    }
}

void ProxyServer::check_idle() {
    if (cfg_.idle_timeout_sec <= 0) return;
    auto now = Clock::now();
    for (auto& kv : clients_) {
        ClientConn* c = kv.second.get();
        if (c->closed || !c->pending.empty()) continue;  // 有在途请求不算空闲
        long idle_for = std::chrono::duration_cast<std::chrono::seconds>(now - c->last_activity).count();
        if (idle_for >= cfg_.idle_timeout_sec) {
            close_client(c, CloseReason::IdleTimeout,
                         "idle for " + std::to_string(idle_for) + "s");
        }
    }
}

void ProxyServer::begin_shutdown() {
    shutting_down_ = true;
    if (listen_fd_ >= 0) {  // 停止接收新连接
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    grace_deadline_ = Clock::now() + std::chrono::seconds(cfg_.grace_sec);
    Logger::info(TAG, "SIGTERM received: stop accepting new connections, draining " +
                 std::to_string(count_inflight()) + " in-flight request(s) (grace=" +
                 std::to_string(cfg_.grace_sec) + "s)");
    // 没有在途请求的空闲连接直接关闭
    for (auto& kv : clients_) {
        ClientConn* c = kv.second.get();
        if (!c->closed && c->pending.empty() && c->out_off >= c->outbuf.size()) {
            close_client(c, CloseReason::ServerShutdown, "idle while draining");
        }
    }
}

void ProxyServer::close_client(ClientConn* c, CloseReason reason, const std::string& detail) {
    if (c->closed) return;
    c->closed = true;
    ::close(c->fd);
    std::string msg = "conn#" + std::to_string(c->cid) + " closed: reason=" + reason_token(reason);
    if (!detail.empty()) msg += " (" + detail + ")";
    if (!c->pending.empty()) {
        msg += ", " + std::to_string(c->pending.size()) + " pending request(s) dropped";
    }
    if (reason == CloseReason::ClientDisconnect) {
        Logger::info(TAG, msg);
    } else {
        Logger::warn(TAG, msg);
    }
}

void ProxyServer::reap_closed() {
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (it->second->closed) {
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

int ProxyServer::compute_timeout_ms() const {
    auto now = Clock::now();
    long best = 1000;  // 默认 1s 兜底节拍
    auto consider = [&](Clock::time_point tp) {
        long ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp - now).count();
        if (ms < 0) ms = 0;
        if (ms < best) best = ms;
    };
    for (const auto& kv : clients_) {
        const ClientConn* c = kv.second.get();
        if (c->closed) continue;
        for (const auto& p : c->pending) {
            if (p.timed && !p.ready) consider(p.deadline);
        }
        if (!shutting_down_ && cfg_.idle_timeout_sec > 0 && c->pending.empty()) {
            consider(c->last_activity + std::chrono::seconds(cfg_.idle_timeout_sec));
        }
    }
    if (shutting_down_) consider(grace_deadline_);
    return static_cast<int>(best);
}

std::size_t ProxyServer::count_inflight() const {
    std::size_t n = 0;
    for (const auto& kv : clients_) {
        if (!kv.second->closed) n += kv.second->pending.size();
    }
    return n;
}

int ProxyServer::run() {
    if (cfg_.path.size() >= sizeof(((struct sockaddr_un*)nullptr)->sun_path)) {
        Logger::error(TAG, "socket 路径过长: " + cfg_.path);
        return SERVE_ERR_GENERIC;
    }
    if (::pipe(g_self_pipe) != 0) {
        Logger::error(TAG, std::string("创建自管道失败: ") + strerror(errno));
        return SERVE_ERR_GENERIC;
    }
    set_non_blocking(g_self_pipe[0]);
    set_non_blocking(g_self_pipe[1]);
    set_cloexec(g_self_pipe[0]);
    set_cloexec(g_self_pipe[1]);
    if (!install_signal_handlers()) {
        Logger::error(TAG, std::string("安装信号处理失败: ") + strerror(errno));
        return SERVE_ERR_GENERIC;
    }

    int prc = prepare_path();
    if (prc != SERVE_OK) return prc;

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        Logger::error(TAG, std::string("创建 socket 失败: ") + strerror(errno));
        return SERVE_ERR_GENERIC;
    }
    set_cloexec(listen_fd_);
    set_non_blocking(listen_fd_);

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, cfg_.path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        Logger::error(TAG, "bind 失败 " + cfg_.path + ": " + strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return SERVE_ERR_PATH_BUSY;
    }
    bound_ = true;

    // 权限限制为当前用户可读写
    if (::chmod(cfg_.path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        Logger::error(TAG, "chmod 0600 失败: " + std::string(strerror(errno)));
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(cfg_.path.c_str());
        return SERVE_ERR_GENERIC;
    }
    if (::listen(listen_fd_, cfg_.backlog) != 0) {
        Logger::error(TAG, std::string("listen 失败: ") + strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(cfg_.path.c_str());
        return SERVE_ERR_GENERIC;
    }

    Logger::info(TAG, "listening on " + cfg_.path +
                 " (max_frame=" + std::to_string(cfg_.max_frame) +
                 " idle_timeout=" + std::to_string(cfg_.idle_timeout_sec) + "s" +
                 " grace=" + std::to_string(cfg_.grace_sec) + "s)");

    // ------------------------- 事件循环 -------------------------
    while (true) {
        if (g_got_term && !shutting_down_) begin_shutdown();
        if (shutting_down_ && clients_.empty()) break;

        std::vector<pollfd> pfds;
        std::vector<ClientConn*> conns;
        pfds.push_back({g_self_pipe[0], POLLIN, 0});
        const bool had_listen = !shutting_down_ &&
                                Clock::now() >= accept_cooldown_until_;
        if (had_listen) pfds.push_back({listen_fd_, POLLIN, 0});
        for (auto& kv : clients_) {
            ClientConn* c = kv.second.get();
            short ev = 0;
            if (!shutting_down_) ev |= POLLIN;  // 退出阶段不再接收新请求
            if (c->out_off < c->outbuf.size()) ev |= POLLOUT;
            pfds.push_back({c->fd, ev, 0});
            conns.push_back(c);
        }

        int r = ::poll(pfds.data(), pfds.size(), compute_timeout_ms());
        if (r < 0) {
            if (errno == EINTR) continue;
            Logger::error(TAG, std::string("poll 失败: ") + strerror(errno));
            exit_code_ = SERVE_ERR_GENERIC;
            break;
        }

        std::size_t pos = 0;
        if (pfds[pos].revents & POLLIN) {  // 自管道: SIGTERM 唤醒
            char drain[64];
            while (::read(g_self_pipe[0], drain, sizeof(drain)) > 0) {}
        }
        ++pos;
        if (g_got_term && !shutting_down_) begin_shutdown();
        if (had_listen) {
            if (!shutting_down_ && (pfds[pos].revents & POLLIN)) accept_all();
            ++pos;
        }

        for (std::size_t i = 0; i < conns.size(); ++i) {
            ClientConn* c = conns[i];
            short rev = pfds[pos + i].revents;
            if (c->closed) continue;
            if (rev & POLLIN) do_read(c);
            if (!c->closed && (rev & POLLOUT)) do_write(c);
            if (!c->closed && (rev & (POLLERR | POLLHUP | POLLNVAL))) {
                close_client(c, CloseReason::ClientDisconnect, "socket hangup/error");
            }
        }

        process_timers();
        if (!shutting_down_) check_idle();

        if (shutting_down_) {
            // 已完成在途请求且发送缓冲清空的连接可以关闭了
            for (auto& kv : clients_) {
                ClientConn* c = kv.second.get();
                if (!c->closed && c->pending.empty() && c->out_off >= c->outbuf.size()) {
                    close_client(c, CloseReason::ServerShutdown, "idle while draining");
                }
            }
        }
        reap_closed();

        if (shutting_down_) {
            if (clients_.empty()) break;  // 全部在途请求完成
            if (Clock::now() >= grace_deadline_) {
                Logger::error(TAG, "grace period expired: " +
                              std::to_string(clients_.size()) + " connection(s), " +
                              std::to_string(count_inflight()) + " request(s) still pending");
                exit_code_ = SERVE_ERR_GRACE_EXPIRED;
                break;
            }
        }
    }

    // ------------------------- 清理 -------------------------
    for (auto& kv : clients_) {
        if (!kv.second->closed) ::close(kv.second->fd);
    }
    clients_.clear();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    ::close(g_self_pipe[0]);
    ::close(g_self_pipe[1]);
    g_self_pipe[0] = g_self_pipe[1] = -1;
    if (bound_) {
        if (::unlink(cfg_.path.c_str()) == 0) {
            Logger::info(TAG, "socket file removed: " + cfg_.path);
        } else {
            Logger::warn(TAG, "删除 socket 文件失败: " + cfg_.path + ": " + strerror(errno));
        }
    }
    if (exit_code_ == SERVE_OK) {
        Logger::info(TAG, "shutdown complete (graceful): requests=" +
                     std::to_string(total_requests_) +
                     " responses=" + std::to_string(total_responses_));
    } else {
        Logger::error(TAG, "shutdown incomplete, exit code " + std::to_string(exit_code_));
    }
    return exit_code_;
}

void print_serve_usage() {
    std::cout <<
        "用法: ipc_demo serve [选项]\n"
        "  --path PATH          监听 Socket 路径 (默认 /tmp/ipc_proxy.sock)\n"
        "  --max-frame BYTES    单帧最大字节数，超限关闭连接 (默认 1048576)\n"
        "  --idle-timeout SEC   空闲连接超时秒数，0 表示不限制 (默认 60)\n"
        "  --grace SEC          SIGTERM 后等待在途请求的宽限期 (默认 5)\n"
        "  --backlog N          listen 积压 (默认 64)\n"
        "  --help               显示本说明\n"
        "退出码: 0=优雅退出 1=一般错误 2=路径被占用 3=宽限期超期\n";
}

} // namespace

int run_socket_serve(const ServeConfig& cfg) {
    return ProxyServer(cfg).run();
}

int socket_serve_main(int argc, char** argv) {
    ServeConfig cfg;
    try {
        for (int i = 0; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&](const char* name) -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(std::string("缺少参数值: ") + name);
                return argv[++i];
            };
            if (a == "--path") {
                cfg.path = value("--path");
            } else if (a == "--max-frame") {
                cfg.max_frame = static_cast<uint32_t>(std::stoul(value("--max-frame")));
            } else if (a == "--idle-timeout") {
                cfg.idle_timeout_sec = std::stoi(value("--idle-timeout"));
            } else if (a == "--grace") {
                cfg.grace_sec = std::stoi(value("--grace"));
            } else if (a == "--backlog") {
                cfg.backlog = std::stoi(value("--backlog"));
            } else if (a == "--help" || a == "-h") {
                print_serve_usage();
                return SERVE_OK;
            } else {
                std::cerr << "未知参数: " << a << std::endl;
                print_serve_usage();
                return SERVE_ERR_GENERIC;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "参数解析失败: " << e.what() << std::endl;
        print_serve_usage();
        return SERVE_ERR_GENERIC;
    }
    if (cfg.max_frame == 0) {
        std::cerr << "--max-frame 必须大于 0" << std::endl;
        return SERVE_ERR_GENERIC;
    }
    return run_socket_serve(cfg);
}

} // namespace ipc
