/**
 * unix_socket_agent.h
 *
 * Unix Domain Socket 长度前缀帧协议（固定网络字节序 / 大端序）
 *
 * 帧格式（所有整数字段均为大端序）：
 *
 *   uint32 frame_length    整帧长度（含该字段本身），最小 16
 *   uint64 request_id      请求标识，由客户端分配
 *   uint8  type            帧类型：1=REQUEST 2=RESPONSE 3=ERROR
 *   uint8  flags           保留标志位（合法值：0）
 *   uint16 reserved        保留字段（合法值：0）
 *   uint8  payload[]       长度 = frame_length - 16
 *
 * 特点：
 * - write_full / read_frame_* 正确处理短写、EINTR、拆包与粘包
 * - 帧长超过上限、协议字段非法、空闲超时都有可区分的错误码
 * - serve / request 两个独立工作模式共享同一套编解码实现
 *
 * 仅依赖 Linux/POSIX，无第三方依赖。
 */
#ifndef IPC_UNIX_SOCKET_AGENT_H
#define IPC_UNIX_SOCKET_AGENT_H

#include <cstdint>
#include <cstring>
#include <poll.h>
#include <string>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace ipc {
namespace uxsock {

// ---- 协议常量 ---------------------------------------------------------------

constexpr uint32_t kHeaderSize = 16;               // 4 + 8 + 1 + 1 + 2
constexpr uint32_t kDefaultMaxFrame = 1u << 20;    // 默认帧上限 1 MiB
constexpr uint32_t kMaxFrameHardLimit = 64u << 20; // 可配置上限的硬上限 64 MiB

enum FrameType : uint8_t {
    FRAME_REQUEST = 1,
    FRAME_RESPONSE = 2,
    FRAME_ERROR = 3,
};

// 关闭连接 / 通信失败的具体原因（用于日志与退出码区分）
enum class ProtocolError {
    None = 0,
    IoError,          // 读写错误（对端 RST 等）
    PeerClosed,       // 对端关闭（含帧读到一半的中途断开）
    IdleTimeout,      // 超过空闲超时仍无数据
    LengthTooLarge,   // 声明帧长超过本端上限
    LengthTooSmall,   // 声明帧长短于固定头部
    BadFlags,         // flags 字段非法
    BadReserved,      // reserved 字段非 0
    BadType,          // type 字段非法
};

inline const char* protocol_error_name(ProtocolError e) {
    switch (e) {
        case ProtocolError::None: return "ok";
        case ProtocolError::IoError: return "io-error";
        case ProtocolError::PeerClosed: return "peer-closed";
        case ProtocolError::IdleTimeout: return "idle-timeout";
        case ProtocolError::LengthTooLarge: return "frame-too-large";
        case ProtocolError::LengthTooSmall: return "frame-length-invalid";
        case ProtocolError::BadFlags: return "bad-flags";
        case ProtocolError::BadReserved: return "bad-reserved";
        case ProtocolError::BadType: return "bad-type";
    }
    return "unknown";
}

struct Frame {
    uint64_t request_id = 0;
    uint8_t type = FRAME_REQUEST;
    uint8_t flags = 0;
    std::string payload;
};

struct ServerConfig {
    std::string path = "/tmp/ipc_agent.sock";
    uint32_t max_frame_bytes = kDefaultMaxFrame;
    int idle_timeout_ms = 30000; // 连接空闲超时，<=0 表示不限
    int grace_ms = 5000;         // SIGTERM 后的关停宽限期
    int backlog = 64;
};

// ---- 字节序（大端 / 网络字节序）--------------------------------------------

inline void put_u16_be(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

inline void put_u32_be(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline void put_u64_be(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (56 - i * 8));
    }
}

inline uint16_t get_u16_be(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                 static_cast<uint16_t>(p[1]));
}

inline uint32_t get_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline uint64_t get_u64_be(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<uint64_t>(p[i]);
    }
    return v;
}

// ---- 阻塞 IO 原语（处理短写、EINTR）-----------------------------------------

// 完整写入 buf；处理短写与 EINTR。失败时 errno 保留（EPIPE/ECONNRESET 表示
// 对端断开）。
inline bool write_full(int fd, const void* buf, size_t len) {
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// 等待 fd 可读，timeout_ms 为相对超时；<0 表示永久等待。
// 返回：1 可读；0 超时；-1 出错；-2 被信号中断。
inline int wait_readable(int fd, int timeout_ms) {
    for (;;) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc > 0) return 1;
        if (rc == 0) return 0;
        if (errno == EINTR) return -2;
        return -1;
    }
}

// 读取最多 len 字节到 buf（一次 recv）。返回 None / PeerClosed /
// IdleTimeout / IoError。
inline ProtocolError recv_some(int fd, uint8_t* buf, size_t len,
                               ssize_t* nread, int timeout_ms) {
    int rc = wait_readable(fd, timeout_ms);
    if (rc == 0) return ProtocolError::IdleTimeout;
    if (rc == -1 || rc == -2) return ProtocolError::IoError;

    for (;;) {
        ssize_t n = ::recv(fd, buf, len, 0);
        if (n >= 0) {
            *nread = n;
            return n == 0 ? ProtocolError::PeerClosed : ProtocolError::None;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ProtocolError::IdleTimeout;
        }
        return ProtocolError::IoError;
    }
}

// ---- 帧编解码 ---------------------------------------------------------------

inline bool encode_frame(const Frame& f, uint32_t max_frame_bytes,
                         std::vector<uint8_t>* out) {
    if (max_frame_bytes < kHeaderSize ||
        f.payload.size() >
            static_cast<size_t>(max_frame_bytes - kHeaderSize)) {
        return false;
    }
    const uint32_t total =
        kHeaderSize + static_cast<uint32_t>(f.payload.size());
    out->resize(total);
    uint8_t* p = out->data();
    put_u32_be(p, total);
    put_u64_be(p + 4, f.request_id);
    p[12] = f.type;
    p[13] = f.flags;
    put_u16_be(p + 14, 0);
    if (!f.payload.empty()) {
        std::memcpy(p + kHeaderSize, f.payload.data(), f.payload.size());
    }
    return true;
}

inline bool write_frame(int fd, const Frame& f, uint32_t max_frame_bytes) {
    std::vector<uint8_t> buf;
    if (!encode_frame(f, max_frame_bytes, &buf)) return false;
    return write_full(fd, buf.data(), buf.size());
}

// 帧读取器：内部维护读缓冲，天然支持拆包（一次 recv 不足一帧）与粘包
// （一次 recv 携带多帧）。超时按“距上一次收到任何字节”计算。
class FrameReader {
public:
    FrameReader() = default;

    size_t buffered_bytes() const { return buf_.size(); }

    ProtocolError read_frame(int fd, uint32_t max_frame_bytes, Frame* out,
                             int idle_timeout_ms) {
        // 缓冲区里还凑不齐完整帧时持续收数据
        while (true) {
            bool have_frame = false;
            ProtocolError pe = try_consume(max_frame_bytes, out,
                                           &have_frame);
            if (have_frame || pe != ProtocolError::None) return pe;

            uint8_t chunk[8192];
            ssize_t n = 0;
            pe = recv_some(fd, chunk, sizeof(chunk), &n, idle_timeout_ms);
            if (pe != ProtocolError::None) return pe;
            buf_.insert(buf_.end(), chunk, chunk + n);
            // 仅凭已有字节就能判定帧长超限，立即拒绝而不必继续读
            if (buf_.size() >= 4 &&
                get_u32_be(buf_.data()) > max_frame_bytes) {
                return ProtocolError::LengthTooLarge;
            }
            if (buf_.size() > static_cast<size_t>(max_frame_bytes) + 16) {
                return ProtocolError::LengthTooLarge;
            }
        }
    }

private:
    // 尝试从缓冲区解析一帧。have_frame=false 且返回 None 表示数据不足。
    ProtocolError try_consume(uint32_t max_frame_bytes, Frame* out,
                              bool* have_frame) {
        *have_frame = false;
        if (buf_.size() < 4) return ProtocolError::None;
        const uint32_t total = get_u32_be(buf_.data());
        if (total < kHeaderSize) return ProtocolError::LengthTooSmall;
        if (total > max_frame_bytes) return ProtocolError::LengthTooLarge;
        if (buf_.size() < total) return ProtocolError::None;

        const uint8_t* hdr = buf_.data();
        const uint8_t type = hdr[12];
        const uint8_t flags = hdr[13];
        const uint16_t reserved = get_u16_be(hdr + 14);
        if (type != FRAME_REQUEST && type != FRAME_RESPONSE &&
            type != FRAME_ERROR) {
            return ProtocolError::BadType;
        }
        if (flags != 0) return ProtocolError::BadFlags;
        if (reserved != 0) return ProtocolError::BadReserved;

        out->request_id = get_u64_be(hdr + 4);
        out->type = type;
        out->flags = flags;
        out->payload.assign(reinterpret_cast<const char*>(hdr + kHeaderSize),
                            static_cast<size_t>(total - kHeaderSize));
        buf_.erase(buf_.begin(), buf_.begin() + total);
        *have_frame = true;
        return ProtocolError::None;
    }

    std::vector<uint8_t> buf_;
};

// ---- Socket 辅助函数 --------------------------------------------------------

// 若路径已被占用，只在确认是一个“当前无人监听”的 Socket 节点时移除；
// 占用者仍在监听（或节点不是 Socket）时返回 false，绝不误删。
inline bool reclaim_stale_socket(const std::string& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT; // 不存在，可直接绑定
    }
    if (!S_ISSOCK(st.st_mode)) return false;

    int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe < 0) return false;

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    bool stale = false;
    if (::connect(probe, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) == 0) {
        stale = false; // 对端 accept 成功，Socket 正在服役
    } else if (errno == ECONNREFUSED) {
        stale = true;  // 无监听者，典型的残留节点
    }
    ::close(probe);

    if (!stale) return false;
    return ::unlink(path.c_str()) == 0;
}

// 绑定并监听；成功后把节点权限收紧为 0600（仅当前用户可读写）。
inline int create_listening_socket(const ServerConfig& cfg,
                                   std::string* err) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (err) *err = std::string("socket: ") + std::strerror(errno);
        return -1;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (cfg.path.size() >= sizeof(addr.sun_path)) {
        if (err) *err = "socket path too long";
        ::close(fd);
        return -1;
    }
    std::strncpy(addr.sun_path, cfg.path.c_str(),
                 sizeof(addr.sun_path) - 1);

    auto do_bind = [&] {
        return ::bind(fd, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr));
    };

    if (do_bind() != 0) {
        if (errno != EADDRINUSE || !reclaim_stale_socket(cfg.path) ||
            do_bind() != 0) {
            if (err)
                *err = "bind " + cfg.path +
                       ": " + std::string(std::strerror(errno)) +
                       " (live socket or non-socket file at path)";
            ::close(fd);
            return -1;
        }
    }

    // 节点在 bind 时按 umask 创建，这里显式收紧为仅属主可读写。
    if (::chmod(cfg.path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        if (err) *err = std::string("chmod: ") + std::strerror(errno);
        ::close(fd);
        ::unlink(cfg.path.c_str());
        return -1;
    }

    if (::listen(fd, cfg.backlog) != 0) {
        if (err) *err = std::string("listen: ") + std::strerror(errno);
        ::close(fd);
        ::unlink(cfg.path.c_str());
        return -1;
    }
    return fd;
}

// 连接服务端，在 timeout_ms 内重试等待就绪；<0 表示一直重试。
inline int connect_to_server(const std::string& path, int timeout_ms,
                             std::string* err) {
    const int interval_ms = 20;
    int waited = 0;
    for (;;) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            if (err) *err = std::string("socket: ") + std::strerror(errno);
            return -1;
        }
        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(),
                     sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr)) == 0) {
            return fd;
        }
        ::close(fd);
        if (timeout_ms >= 0 && waited >= timeout_ms) {
            if (err)
                *err = "connect to " + path + " timed out after " +
                       std::to_string(timeout_ms) + " ms";
            return -1;
        }
        struct timespec ts = {0, interval_ms * 1000 * 1000};
        nanosleep(&ts, nullptr);
        waited += interval_ms;
    }
}

// ---- 工作模式入口（实现在 unix_socket_agent.cpp）----------------------------

struct RequestOptions {
    std::string path = "/tmp/ipc_agent.sock";
    // 每条请求一项；请求按顺序发送，响应也按 request_id 顺序返回
    std::vector<std::string> payloads;
    // 协议探针模式：原样发送的十六进制字节串（与 payloads 二选一）
    std::vector<std::string> raw_blobs;
    uint32_t max_frame_bytes = kDefaultMaxFrame;
    int connect_timeout_ms = 5000;
    bool byte_at_a_time = false; // 测试用：逐字节发送以制造半包
    int delay_us = 0;            // 测试用：每次发送间隔
};

// 启动服务端。返回进程退出码（0 成功，非 0 失败）。
int run_serve(const ServerConfig& cfg);

// 启动客户端（request 模式）。响应通过 stdout 的 "RESPONSE <id> <payload>"
// 行输出；返回进程退出码。
int run_request(const RequestOptions& opts);

} // namespace uxsock
} // namespace ipc

#endif // IPC_UNIX_SOCKET_AGENT_H
