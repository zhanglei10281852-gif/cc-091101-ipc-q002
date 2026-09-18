/**
 * C++ 进程间通信 (IPC) 演示程序
 * 
 * 本程序演示了 6 种主要的 IPC 方式：
 * 1. 管道 (Pipe)
 * 2. 命名管道 (Named Pipe / FIFO)
 * 3. 共享内存 (Shared Memory)
 * 4. 消息队列 (Message Queue)
 * 5. 信号 (Signal)
 * 6. Socket (Unix Domain Socket)
 */

#include "ipc_demo.h"
#include "socket_proxy.h"

void print_usage() {
    std::cout << "用法:" << std::endl;
    std::cout << "  ipc_demo                 交互式菜单 (含原 Socket 演示)" << std::endl;
    std::cout << "  ipc_demo --all           运行全部 IPC 演示" << std::endl;
    std::cout << "  ipc_demo serve [选项]    启动本地代理服务端 (多客户端 Unix Socket)" << std::endl;
    std::cout << "  ipc_demo request [选项]  启动本地代理客户端" << std::endl;
    std::cout << "  ipc_demo serve --help / ipc_demo request --help  查看详细选项" << std::endl;
}

void print_menu() {
    std::cout << "\n\033[35m╔════════════════════════════════════════════╗\033[0m" << std::endl;
    std::cout << "\033[35m║   C++ 进程间通信 (IPC) 演示程序            ║\033[0m" << std::endl;
    std::cout << "\033[35m╠════════════════════════════════════════════╣\033[0m" << std::endl;
    std::cout << "\033[35m║  1. 管道 (Pipe)                            ║\033[0m" << std::endl;
    std::cout << "\033[35m║  2. 命名管道 (Named Pipe / FIFO)           ║\033[0m" << std::endl;
    std::cout << "\033[35m║  3. 共享内存 (Shared Memory)               ║\033[0m" << std::endl;
    std::cout << "\033[35m║  4. 消息队列 (Message Queue)               ║\033[0m" << std::endl;
    std::cout << "\033[35m║  5. 信号 (Signal)                          ║\033[0m" << std::endl;
    std::cout << "\033[35m║  6. Socket (Unix Domain Socket)            ║\033[0m" << std::endl;
    std::cout << "\033[35m║  7. 运行所有演示                           ║\033[0m" << std::endl;
    std::cout << "\033[35m║  0. 退出                                   ║\033[0m" << std::endl;
    std::cout << "\033[35m╚════════════════════════════════════════════╝\033[0m" << std::endl;
    std::cout << "  本地代理模式: ipc_demo serve | ipc_demo request (--help 查看选项)" << std::endl;
    std::cout << "\n请选择 (0-7): ";
}

void run_all_demos() {
    ipc::Logger::demo("运行所有 IPC 演示");
    
    ipc::demo_pipe();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    ipc::demo_named_pipe();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    ipc::demo_shared_memory();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    ipc::demo_message_queue();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    ipc::demo_signal();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    ipc::demo_socket();
    
    ipc::Logger::info("MAIN", "所有演示完成！");
}

int main(int argc, char* argv[]) {
    if (argc > 1) {
        std::string cmd = argv[1];
        // 如果有命令行参数 --all，直接运行所有演示
        if (cmd == "--all") {
            run_all_demos();
            return 0;
        }
        // 本地代理工作模式: 可分别启动的独立服务端 / 客户端
        if (cmd == "serve") {
            return ipc::socket_serve_main(argc - 2, argv + 2);
        }
        if (cmd == "request") {
            return ipc::socket_request_main(argc - 2, argv + 2);
        }
        if (cmd == "--help" || cmd == "-h") {
            print_usage();
            return 0;
        }
    }

    int choice;
    
    while (true) {
        print_menu();
        std::cin >> choice;
        
        if (std::cin.fail()) {
            std::cin.clear();
            std::cin.ignore(10000, '\n');
            ipc::Logger::warn("MAIN", "无效输入，请输入数字");
            continue;
        }
        
        switch (choice) {
            case 0:
                ipc::Logger::info("MAIN", "程序退出，再见！");
                return 0;
            case 1:
                ipc::demo_pipe();
                break;
            case 2:
                ipc::demo_named_pipe();
                break;
            case 3:
                ipc::demo_shared_memory();
                break;
            case 4:
                ipc::demo_message_queue();
                break;
            case 5:
                ipc::demo_signal();
                break;
            case 6:
                ipc::demo_socket();
                break;
            case 7:
                run_all_demos();
                break;
            default:
                ipc::Logger::warn("MAIN", "无效选择，请输入 0-7");
        }
    }
    
    return 0;
}
