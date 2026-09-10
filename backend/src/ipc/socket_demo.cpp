/**
 * Unix Domain Socket 演示
 * 
 * Unix Domain Socket 是一种本地进程间通信方式，类似网络 Socket 但更高效。
 * 特点：
 * - 支持双向通信
 * - 支持流式 (SOCK_STREAM) 和数据报 (SOCK_DGRAM) 两种模式
 * - 比网络 Socket 更高效（无需网络协议栈）
 */

#include "ipc_demo.h"

namespace ipc {

void demo_socket() {
    Logger::demo("6. Unix Domain Socket 演示");
    
    Logger::info("SOCKET", "Unix Domain Socket 特点：");
    Logger::info("SOCKET", "  - 支持双向通信");
    Logger::info("SOCKET", "  - 流式 (STREAM) 和数据报 (DGRAM) 模式");
    Logger::info("SOCKET", "  - 比网络 Socket 更高效\n");
    
    const char* socket_path = "/tmp/ipc_demo_socket";
    
    // 删除可能存在的旧 socket 文件
    unlink(socket_path);
    
    pid_t pid = fork();
    
    if (pid == -1) {
        Logger::error("SOCKET", "fork 失败");
        return;
    }
    
    if (pid == 0) {
        // 子进程：客户端
        usleep(200000);  // 等待服务器启动
        
        Logger::info("SOCKET-CLIENT", "客户端启动...");
        
        // 创建 socket
        int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client_fd == -1) {
            Logger::error("SOCKET-CLIENT", "创建 socket 失败");
            _exit(1);
        }
        
        // 设置服务器地址
        struct sockaddr_un server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sun_family = AF_UNIX;
        strncpy(server_addr.sun_path, socket_path, sizeof(server_addr.sun_path) - 1);
        
        // 连接服务器
        if (connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
            Logger::error("SOCKET-CLIENT", "连接服务器失败: " + std::string(strerror(errno)));
            close(client_fd);
            _exit(1);
        }
        
        Logger::info("SOCKET-CLIENT", "已连接到服务器\n");
        
        // 发送消息并接收响应
        const char* messages[] = {
            "Hello Server!",
            "How are you?",
            "This is Unix Domain Socket",
            "Goodbye!"
        };
        
        char buffer[256];
        
        for (const char* msg : messages) {
            // 发送
            Logger::info("SOCKET-CLIENT", "发送: " + std::string(msg));
            send(client_fd, msg, strlen(msg) + 1, 0);
            
            // 接收响应
            ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);
            if (bytes > 0) {
                Logger::info("SOCKET-CLIENT", "收到响应: " + std::string(buffer));
            }
            
            usleep(150000);
        }
        
        close(client_fd);
        Logger::info("SOCKET-CLIENT", "客户端关闭连接");
        _exit(0);
        
    } else {
        // 父进程：服务器
        Logger::info("SOCKET-SERVER", "服务器启动...");
        
        // 创建 socket
        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd == -1) {
            Logger::error("SOCKET-SERVER", "创建 socket 失败");
            waitpid(pid, nullptr, 0);
            return;
        }
        
        // 设置服务器地址
        struct sockaddr_un server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sun_family = AF_UNIX;
        strncpy(server_addr.sun_path, socket_path, sizeof(server_addr.sun_path) - 1);
        
        // 绑定
        if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
            Logger::error("SOCKET-SERVER", "bind 失败: " + std::string(strerror(errno)));
            close(server_fd);
            waitpid(pid, nullptr, 0);
            return;
        }
        
        // 监听
        if (listen(server_fd, 5) == -1) {
            Logger::error("SOCKET-SERVER", "listen 失败");
            close(server_fd);
            unlink(socket_path);
            waitpid(pid, nullptr, 0);
            return;
        }
        
        Logger::info("SOCKET-SERVER", "服务器监听中: " + std::string(socket_path));
        
        // 接受连接
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd == -1) {
            Logger::error("SOCKET-SERVER", "accept 失败");
            close(server_fd);
            unlink(socket_path);
            waitpid(pid, nullptr, 0);
            return;
        }
        
        Logger::info("SOCKET-SERVER", "客户端已连接\n");
        
        // 接收消息并发送响应
        char buffer[256];
        int msg_count = 0;
        
        while (true) {
            ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);
            if (bytes <= 0) {
                break;
            }
            
            msg_count++;
            Logger::info("SOCKET-SERVER", "收到: " + std::string(buffer));
            
            // 构造响应
            std::string response = "Server ACK #" + std::to_string(msg_count) + 
                                  ": Received '" + std::string(buffer) + "'";
            send(client_fd, response.c_str(), response.length() + 1, 0);
            Logger::info("SOCKET-SERVER", "发送响应: " + response);
        }
        
        // 清理
        close(client_fd);
        close(server_fd);
        unlink(socket_path);
        
        // 等待子进程
        int status;
        waitpid(pid, &status, 0);
        
        Logger::info("SOCKET-SERVER", "服务器关闭，共处理 " + 
                    std::to_string(msg_count) + " 条消息");
    }
    
    Logger::info("SOCKET", "Socket 演示完成\n");
}

} // namespace ipc
