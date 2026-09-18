#ifndef SOCKET_PROXY_H
#define SOCKET_PROXY_H

/**
 * Unix Domain Socket 本地代理 (serve / request 工作模式)
 *
 * serve    - 启动服务端，可同时维护多个客户端连接
 * request  - 启动客户端，向服务端发送一组请求并校验响应
 *
 * 两种模式可作为独立进程分别启动，配合 tests/test_socket_proxy.cpp
 * 中的 Linux 集成测试观察并发、拆包/粘包、超限拒绝与优雅退出行为。
 */

#include <cstddef>
#include <cstdint>
#include <string>

namespace ipc {

// serve 模式退出码
enum ServeExitCode {
    SERVE_OK = 0,                 // 收到 SIGTERM 后优雅退出
    SERVE_ERR_GENERIC = 1,        // 参数/系统调用等一般错误
    SERVE_ERR_PATH_BUSY = 2,      // 路径被占用 (非失效 Socket) 或 bind 失败
    SERVE_ERR_GRACE_EXPIRED = 3,  // 宽限期内仍有未完成请求，明确失败
};

// request 模式退出码
enum RequestExitCode {
    REQ_OK = 0,                    // 全部响应按序到达且内容校验通过
    REQ_ERR_USAGE = 1,             // 参数错误
    REQ_ERR_CONNECT = 2,           // 连接服务端失败
    REQ_ERR_IO = 3,                // 读写系统调用错误
    REQ_ERR_CLOSED_BY_SERVER = 4,  // 连接被服务端关闭 (超限/超时/协议错误等)
    REQ_ERR_MISMATCH = 5,          // 响应 id 乱序或内容校验失败
    REQ_ERR_TIMEOUT = 6,           // 等待响应超时
};

struct ServeConfig {
    std::string path = "/tmp/ipc_proxy.sock";  // 监听 Socket 路径
    uint32_t max_frame = 1u << 20;             // 单帧最大字节数，超限关闭连接
    int idle_timeout_sec = 60;                 // 空闲连接超时秒数，0 = 不限制
    int grace_sec = 5;                         // SIGTERM 后等待在途请求的宽限期
    int backlog = 64;                          // listen 积压
    std::size_t max_out_buf = 8u << 20;        // 单连接发送缓冲上限 (慢消费者保护)
};

struct RequestConfig {
    std::string path = "/tmp/ipc_proxy.sock";  // 服务端 Socket 路径
    int count = 4;                             // 请求数量
    std::string message = "hello";             // 请求内容
    int payload_size = 0;                      // >0 时生成指定字节的请求内容
    int sleep_ms = -1;                         // >=0 时首个请求要求服务端延迟响应
    bool pipeline = false;                     // 先连续发完全部请求再统一读响应 (粘包)
    int chunk_bytes = 0;                       // >0 时每次只写 N 字节 (拆包)
    int chunk_delay_ms = 0;                    // 写块之间的间隔
    int read_chunk_bytes = 0;                  // >0 时每次只读 N 字节 (慢消费者)
    int read_delay_ms = 0;                     // 读块之间的间隔
    int hold_sec = -1;                         // >=0 时连接后保持沉默 SEC 秒
    uint32_t jumbo_len = 0;                    // >0 时只发送声明该长度的帧头 (超限测试)
    int recv_timeout_ms = 10000;               // 单个响应的接收超时
    std::string client_id;                     // 日志中的客户端标识
};

// 模式入口 (返回上述退出码)
int run_socket_serve(const ServeConfig& cfg);
int run_socket_request(const RequestConfig& cfg);

// 命令行入口: 解析 argv 后调用对应模式
int socket_serve_main(int argc, char** argv);
int socket_request_main(int argc, char** argv);

} // namespace ipc

#endif // SOCKET_PROXY_H
