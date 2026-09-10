#ifndef IPC_DEMO_H
#define IPC_DEMO_H

#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <chrono>
#include <thread>

namespace ipc {

// 日志工具
class Logger {
public:
    static void info(const std::string& tag, const std::string& msg) {
        std::cout << "\033[32m[INFO]\033[0m [" << tag << "] " << msg << std::endl;
    }
    
    static void warn(const std::string& tag, const std::string& msg) {
        std::cout << "\033[33m[WARN]\033[0m [" << tag << "] " << msg << std::endl;
    }
    
    static void error(const std::string& tag, const std::string& msg) {
        std::cerr << "\033[31m[ERROR]\033[0m [" << tag << "] " << msg << std::endl;
    }
    
    static void demo(const std::string& title) {
        std::cout << "\n\033[36m========================================\033[0m" << std::endl;
        std::cout << "\033[36m  " << title << "\033[0m" << std::endl;
        std::cout << "\033[36m========================================\033[0m\n" << std::endl;
    }
};

// 各 IPC 演示函数声明
void demo_pipe();
void demo_named_pipe();
void demo_shared_memory();
void demo_message_queue();
void demo_signal();
void demo_socket();

} // namespace ipc

#endif // IPC_DEMO_H
