/**
 * 信号 (Signal) 演示
 * 
 * 信号是一种异步通知机制，用于通知进程发生了某个事件。
 * 特点：
 * - 异步通信，可以中断进程执行
 * - 信号种类有限，携带信息量少
 * - 常用于进程控制和异常处理
 */

#include "ipc_demo.h"

namespace ipc {

// 全局变量用于信号处理
static volatile sig_atomic_t signal_received = 0;
static volatile sig_atomic_t signal_count = 0;

// 信号处理函数
void signal_handler(int signum) {
    signal_received = signum;
    signal_count++;
}

// SIGUSR1 处理函数
void sigusr1_handler(int signum) {
    // 注意：信号处理函数中应避免使用非异步安全函数
    // 这里仅作演示
    signal_received = signum;
    signal_count++;
}

// SIGUSR2 处理函数
void sigusr2_handler(int signum) {
    signal_received = signum;
    signal_count++;
}

void demo_signal() {
    Logger::demo("5. 信号 (Signal) 演示");
    
    Logger::info("SIGNAL", "信号特点：");
    Logger::info("SIGNAL", "  - 异步通知机制");
    Logger::info("SIGNAL", "  - 可中断进程执行");
    Logger::info("SIGNAL", "  - 常用信号: SIGUSR1, SIGUSR2, SIGTERM 等\n");
    
    // 显示常用信号
    Logger::info("SIGNAL", "常用信号列表:");
    Logger::info("SIGNAL", "  SIGHUP  (1)  - 终端挂起");
    Logger::info("SIGNAL", "  SIGINT  (2)  - 中断 (Ctrl+C)");
    Logger::info("SIGNAL", "  SIGQUIT (3)  - 退出");
    Logger::info("SIGNAL", "  SIGKILL (9)  - 强制终止（不可捕获）");
    Logger::info("SIGNAL", "  SIGUSR1 (10) - 用户自定义信号1");
    Logger::info("SIGNAL", "  SIGUSR2 (12) - 用户自定义信号2");
    Logger::info("SIGNAL", "  SIGTERM (15) - 终止请求");
    Logger::info("SIGNAL", "  SIGCHLD (17) - 子进程状态改变\n");
    
    // 重置计数器
    signal_received = 0;
    signal_count = 0;
    
    pid_t pid = fork();
    
    if (pid == -1) {
        Logger::error("SIGNAL", "fork 失败");
        return;
    }
    
    if (pid == 0) {
        // 子进程：接收信号
        Logger::info("SIGNAL-CHILD", "子进程 PID: " + std::to_string(getpid()));
        
        // 设置信号处理函数
        struct sigaction sa1, sa2;
        
        sa1.sa_handler = sigusr1_handler;
        sigemptyset(&sa1.sa_mask);
        sa1.sa_flags = 0;
        sigaction(SIGUSR1, &sa1, nullptr);
        
        sa2.sa_handler = sigusr2_handler;
        sigemptyset(&sa2.sa_mask);
        sa2.sa_flags = 0;
        sigaction(SIGUSR2, &sa2, nullptr);
        
        Logger::info("SIGNAL-CHILD", "信号处理函数已注册，等待信号...\n");
        
        // 等待信号
        int expected_signals = 5;
        while (signal_count < expected_signals) {
            pause();  // 等待信号
            
            if (signal_received == SIGUSR1) {
                Logger::info("SIGNAL-CHILD", "收到 SIGUSR1 信号！(第 " + 
                            std::to_string(signal_count) + " 个信号)");
            } else if (signal_received == SIGUSR2) {
                Logger::info("SIGNAL-CHILD", "收到 SIGUSR2 信号！(第 " + 
                            std::to_string(signal_count) + " 个信号)");
            }
        }
        
        Logger::info("SIGNAL-CHILD", "共收到 " + std::to_string(signal_count) + " 个信号，退出");
        _exit(0);
        
    } else {
        // 父进程：发送信号
        Logger::info("SIGNAL-PARENT", "父进程 PID: " + std::to_string(getpid()));
        Logger::info("SIGNAL-PARENT", "子进程 PID: " + std::to_string(pid));
        
        usleep(200000);  // 等待子进程设置信号处理
        
        Logger::info("SIGNAL-PARENT", "\n开始发送信号...");
        
        // 发送多个信号
        Logger::info("SIGNAL-PARENT", "发送 SIGUSR1...");
        kill(pid, SIGUSR1);
        usleep(200000);
        
        Logger::info("SIGNAL-PARENT", "发送 SIGUSR2...");
        kill(pid, SIGUSR2);
        usleep(200000);
        
        Logger::info("SIGNAL-PARENT", "发送 SIGUSR1...");
        kill(pid, SIGUSR1);
        usleep(200000);
        
        Logger::info("SIGNAL-PARENT", "发送 SIGUSR2...");
        kill(pid, SIGUSR2);
        usleep(200000);
        
        Logger::info("SIGNAL-PARENT", "发送 SIGUSR1 (最后一个)...");
        kill(pid, SIGUSR1);
        
        // 等待子进程
        int status;
        waitpid(pid, &status, 0);
        
        if (WIFEXITED(status)) {
            Logger::info("SIGNAL-PARENT", "子进程正常退出");
        }
    }
    
    Logger::info("SIGNAL", "信号演示完成\n");
}

} // namespace ipc
