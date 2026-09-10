/**
 * 管道 (Pipe) 演示
 * 
 * 管道是最基本的 IPC 方式，用于父子进程之间的单向通信。
 * 特点：
 * - 半双工通信（数据只能单向流动）
 * - 只能用于有亲缘关系的进程（父子进程）
 * - 管道存在于内存中，不是文件系统的一部分
 */

#include "ipc_demo.h"

namespace ipc {

void demo_pipe() {
    Logger::demo("1. 管道 (Pipe) 演示");
    
    Logger::info("PIPE", "管道特点：");
    Logger::info("PIPE", "  - 半双工通信，数据单向流动");
    Logger::info("PIPE", "  - 仅用于父子进程通信");
    Logger::info("PIPE", "  - 数据存在于内核缓冲区\n");
    
    int pipefd[2];  // pipefd[0] 读端, pipefd[1] 写端
    
    // 创建管道
    if (pipe(pipefd) == -1) {
        Logger::error("PIPE", "创建管道失败: " + std::string(strerror(errno)));
        return;
    }
    
    Logger::info("PIPE", "管道创建成功，准备 fork 子进程...");
    
    pid_t pid = fork();
    
    if (pid == -1) {
        Logger::error("PIPE", "fork 失败: " + std::string(strerror(errno)));
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    
    if (pid == 0) {
        // 子进程：写入数据
        close(pipefd[0]);  // 关闭读端
        
        const char* messages[] = {
            "Hello from child process!",
            "This is message 2",
            "Pipe IPC demonstration",
            "Goodbye!"
        };
        
        for (const char* msg : messages) {
            Logger::info("PIPE-CHILD", "发送: " + std::string(msg));
            write(pipefd[1], msg, strlen(msg) + 1);
            usleep(100000);  // 100ms 延迟
        }
        
        close(pipefd[1]);
        Logger::info("PIPE-CHILD", "子进程完成，退出");
        _exit(0);
        
    } else {
        // 父进程：读取数据
        close(pipefd[1]);  // 关闭写端
        
        char buffer[256];
        ssize_t bytes_read;
        
        Logger::info("PIPE-PARENT", "等待子进程数据...\n");
        
        while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
            Logger::info("PIPE-PARENT", "收到: " + std::string(buffer));
        }
        
        close(pipefd[0]);
        
        // 等待子进程结束
        int status;
        waitpid(pid, &status, 0);
        
        if (WIFEXITED(status)) {
            Logger::info("PIPE-PARENT", "子进程正常退出，退出码: " + 
                        std::to_string(WEXITSTATUS(status)));
        }
    }
    
    Logger::info("PIPE", "管道演示完成\n");
}

} // namespace ipc
