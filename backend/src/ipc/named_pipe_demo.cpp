/**
 * 命名管道 (Named Pipe / FIFO) 演示
 * 
 * 命名管道是一种特殊的文件，可以用于无亲缘关系进程之间的通信。
 * 特点：
 * - 在文件系统中有对应的路径名
 * - 可以用于任意进程之间的通信
 * - 遵循先进先出 (FIFO) 原则
 */

#include "ipc_demo.h"

namespace ipc {

void demo_named_pipe() {
    Logger::demo("2. 命名管道 (Named Pipe / FIFO) 演示");
    
    Logger::info("FIFO", "命名管道特点：");
    Logger::info("FIFO", "  - 在文件系统中有路径名");
    Logger::info("FIFO", "  - 可用于任意进程通信");
    Logger::info("FIFO", "  - 先进先出 (FIFO) 原则\n");
    
    const char* fifo_path = "/tmp/ipc_demo_fifo";
    
    // 删除可能存在的旧 FIFO
    unlink(fifo_path);
    
    // 创建命名管道
    if (mkfifo(fifo_path, 0666) == -1) {
        Logger::error("FIFO", "创建命名管道失败: " + std::string(strerror(errno)));
        return;
    }
    
    Logger::info("FIFO", "命名管道创建成功: " + std::string(fifo_path));
    
    pid_t pid = fork();
    
    if (pid == -1) {
        Logger::error("FIFO", "fork 失败: " + std::string(strerror(errno)));
        unlink(fifo_path);
        return;
    }
    
    if (pid == 0) {
        // 子进程：写入数据
        usleep(100000);  // 等待父进程打开读端
        
        int fd = open(fifo_path, O_WRONLY);
        if (fd == -1) {
            Logger::error("FIFO-WRITER", "打开 FIFO 失败");
            _exit(1);
        }
        
        Logger::info("FIFO-WRITER", "FIFO 写端已打开");
        
        // 发送结构化数据
        struct Message {
            int id;
            char content[64];
        };
        
        Message messages[] = {
            {1, "First message via Named Pipe"},
            {2, "Second message"},
            {3, "Third message"},
            {4, "Final message - END"}
        };
        
        for (const auto& msg : messages) {
            Logger::info("FIFO-WRITER", "发送消息 #" + std::to_string(msg.id) + 
                        ": " + std::string(msg.content));
            write(fd, &msg, sizeof(msg));
            usleep(150000);
        }
        
        close(fd);
        Logger::info("FIFO-WRITER", "写入完成，关闭 FIFO");
        _exit(0);
        
    } else {
        // 父进程：读取数据
        int fd = open(fifo_path, O_RDONLY);
        if (fd == -1) {
            Logger::error("FIFO-READER", "打开 FIFO 失败");
            waitpid(pid, nullptr, 0);
            unlink(fifo_path);
            return;
        }
        
        Logger::info("FIFO-READER", "FIFO 读端已打开，等待数据...\n");
        
        struct Message {
            int id;
            char content[64];
        };
        
        Message msg;
        while (read(fd, &msg, sizeof(msg)) > 0) {
            Logger::info("FIFO-READER", "收到消息 #" + std::to_string(msg.id) + 
                        ": " + std::string(msg.content));
        }
        
        close(fd);
        
        // 等待子进程
        int status;
        waitpid(pid, &status, 0);
        
        // 清理 FIFO 文件
        unlink(fifo_path);
        Logger::info("FIFO", "命名管道已删除");
    }
    
    Logger::info("FIFO", "命名管道演示完成\n");
}

} // namespace ipc
