/**
 * IPC 测试主程序
 */

#include "test_framework.h"

// 外部测试套件声明
void test_pipe_suite();
void test_named_pipe_suite();
void test_shared_memory_suite();
void test_message_queue_suite();
void test_signal_suite();
void test_socket_suite();

int main(int argc, char* argv[]) {
    std::cout << std::endl;
    test::print_cyan("╔══════════════════════════════════════════════════╗\n");
    test::print_cyan("║     C++ IPC (进程间通信) 测试套件                ║\n");
    test::print_cyan("╚══════════════════════════════════════════════════╝\n");
    
    // 运行所有测试套件
    test::run_suite("Pipe (管道)", test_pipe_suite);
    test::run_suite("Named Pipe (命名管道)", test_named_pipe_suite);
    test::run_suite("Shared Memory (共享内存)", test_shared_memory_suite);
    test::run_suite("Message Queue (消息队列)", test_message_queue_suite);
    test::run_suite("Signal (信号)", test_signal_suite);
    test::run_suite("Socket (Unix Domain Socket)", test_socket_suite);
    
    return test::print_summary();
}
