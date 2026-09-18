/**
 * ipc_agent —— Unix Domain Socket 多客户端代理演示入口
 *
 * 用法：
 *   ipc_agent serve [选项]
 *   ipc_agent request [选项] [消息...]
 *
 * serve 选项：
 *   --path PATH             Socket 路径（默认 /tmp/ipc_agent.sock）
 *   --max-frame BYTES       单帧最大字节数（默认 1048576，硬上限 67108864）
 *   --idle-timeout MS       连接空闲超时（默认 30000，0 表示不限）
 *   --grace MS              SIGTERM 后在途请求宽限期（默认 5000）
 *
 * request 选项：
 *   --path PATH             Socket 路径
 *   --max-frame BYTES       本端允许接收的最大帧
 *   --byte-at-a-time        逐字节发送（制造半包）
 *   --delay-us MICROS       逐字节发送时每字节间隔
 *   --send-raw-hex HEX      原样发送十六进制字节（协议探针，可重复）
 *   --hold MS               连接后保持空闲指定毫秒再退出（空闲超时测试）
 *
 * request 无消息且无 --send-raw-hex 时，仅建立连接（配合 --hold）。
 * 结果行输出到 stdout（RESPONSE/ERROR <id> <payload>），日志输出到 stderr。
 */

#include "unix_socket_agent.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace ipc::uxsock;

namespace {

void print_usage() {
    std::printf(
        "Usage:\n"
        "  ipc_agent serve [--path P] [--max-frame N] [--idle-timeout MS]\n"
        "                  [--grace MS]\n"
        "  ipc_agent request [--path P] [--max-frame N] [--byte-at-a-time]\n"
        "                    [--delay-us US] [--hold MS] [MSG...]\n"
        "  ipc_agent request --send-raw-hex HEX [--send-raw-hex HEX ...]\n");
}

bool parse_u32(const char* s, uint32_t* out) {
    char* end = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

bool parse_int(const char* s, int* out) {
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    *out = static_cast<int>(v);
    return true;
}

int run_serve_args(int argc, char** argv) {
    ServerConfig cfg;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--path") {
            const char* v = need_value("--path");
            if (!v) return 1;
            cfg.path = v;
        } else if (a == "--max-frame") {
            const char* v = need_value("--max-frame");
            if (!v || !parse_u32(v, &cfg.max_frame_bytes) ||
                cfg.max_frame_bytes < kHeaderSize ||
                cfg.max_frame_bytes > kMaxFrameHardLimit) {
                std::fprintf(stderr,
                             "invalid --max-frame (allowed %u..%u)\n",
                             kHeaderSize, kMaxFrameHardLimit);
                return 1;
            }
        } else if (a == "--idle-timeout") {
            const char* v = need_value("--idle-timeout");
            if (!v || !parse_int(v, &cfg.idle_timeout_ms) ||
                cfg.idle_timeout_ms < 0) {
                std::fprintf(stderr, "invalid --idle-timeout\n");
                return 1;
            }
        } else if (a == "--grace") {
            const char* v = need_value("--grace");
            if (!v || !parse_int(v, &cfg.grace_ms) || cfg.grace_ms < 0) {
                std::fprintf(stderr, "invalid --grace\n");
                return 1;
            }
        } else {
            std::fprintf(stderr, "unknown serve argument: %s\n", a.c_str());
            print_usage();
            return 1;
        }
    }
    return run_serve(cfg);
}

int run_request_args(int argc, char** argv) {
    RequestOptions opts;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--path") {
            const char* v = need_value("--path");
            if (!v) return 1;
            opts.path = v;
        } else if (a == "--max-frame") {
            const char* v = need_value("--max-frame");
            if (!v || !parse_u32(v, &opts.max_frame_bytes) ||
                opts.max_frame_bytes < kHeaderSize ||
                opts.max_frame_bytes > kMaxFrameHardLimit) {
                std::fprintf(stderr,
                             "invalid --max-frame (allowed %u..%u)\n",
                             kHeaderSize, kMaxFrameHardLimit);
                return 1;
            }
        } else if (a == "--connect-timeout") {
            const char* v = need_value("--connect-timeout");
            if (!v || !parse_int(v, &opts.connect_timeout_ms) ||
                opts.connect_timeout_ms < 0) {
                std::fprintf(stderr, "invalid --connect-timeout\n");
                return 1;
            }
        } else if (a == "--byte-at-a-time") {
            opts.byte_at_a_time = true;
        } else if (a == "--delay-us") {
            const char* v = need_value("--delay-us");
            if (!v || !parse_int(v, &opts.delay_us) || opts.delay_us < 0) {
                std::fprintf(stderr, "invalid --delay-us\n");
                return 1;
            }
        } else if (a == "--hold") {
            const char* v = need_value("--hold");
            int hold = 0;
            if (!v || !parse_int(v, &hold) || hold < 0) {
                std::fprintf(stderr, "invalid --hold\n");
                return 1;
            }
            // 建立连接后保持空闲：若服务端在 hold 内关闭连接（空闲超时），
            // 以退出码 7 报告；hold 到期仍连接则退出 0。
            std::string cerr;
            int cfd = ::connect_to_server(opts.path,
                                          opts.connect_timeout_ms, &cerr);
            if (cfd < 0) {
                std::fprintf(stderr, "%s\n", cerr.c_str());
                return 2;
            }
            struct pollfd pfd;
            pfd.fd = cfd;
            pfd.events = POLLIN;
            int rc = ::poll(&pfd, 1, hold);
            if (rc > 0) {
                char b[16];
                ssize_t n = ::recv(cfd, b, sizeof(b), 0);
                if (n == 0) {
                    std::fprintf(stderr,
                                 "server closed idle connection\n");
                    ::close(cfd);
                    return 7;
                }
            }
            ::close(cfd);
            return 0;
        } else if (a == "--send-raw-hex") {
            const char* v = need_value("--send-raw-hex");
            if (!v) return 1;
            opts.raw_blobs.emplace_back(v);
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown request argument: %s\n",
                         a.c_str());
            print_usage();
            return 1;
        } else {
            opts.payloads.push_back(a);
        }
    }
    return run_request(opts);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    std::string mode = argv[1];
    if (mode == "serve") return run_serve_args(argc - 2, argv + 2);
    if (mode == "request") return run_request_args(argc - 2, argv + 2);
    if (mode == "-h" || mode == "--help" || mode == "help") {
        print_usage();
        return 0;
    }
    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    print_usage();
    return 1;
}
