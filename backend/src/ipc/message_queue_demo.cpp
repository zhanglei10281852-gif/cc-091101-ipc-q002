/**
 * 消息队列 (Message Queue) 演示
 * 
 * 消息队列允许进程以消息为单位进行通信，支持消息类型过滤。
 * 特点：
 * - 消息有类型，可以选择性接收
 * - 消息队列独立于进程存在
 * - 支持多对多通信
 */

#include "ipc_demo.h"

namespace ipc {

// 消息结构
struct MsgBuffer {
    long msg_type;      // 消息类型，必须 > 0
    char msg_text[128]; // 消息内容
    int priority;       // 优先级
};

void demo_message_queue() {
    Logger::demo("4. 消息队列 (Message Queue) 演示");
    
    Logger::info("MSGQ", "消息队列特点：");
    Logger::info("MSGQ", "  - 消息有类型，可选择性接收");
    Logger::info("MSGQ", "  - 独立于进程存在");
    Logger::info("MSGQ", "  - 支持多对多通信\n");
    
    // 创建消息队列
    key_t key = ftok("/tmp", 'M');
    if (key == -1) {
        Logger::error("MSGQ", "ftok 失败: " + std::string(strerror(errno)));
        return;
    }
    
    int msgq_id = msgget(key, IPC_CREAT | 0666);
    if (msgq_id == -1) {
        Logger::error("MSGQ", "msgget 失败: " + std::string(strerror(errno)));
        return;
    }
    
    Logger::info("MSGQ", "消息队列创建成功，ID: " + std::to_string(msgq_id));
    
    pid_t pid = fork();
    
    if (pid == -1) {
        Logger::error("MSGQ", "fork 失败");
        msgctl(msgq_id, IPC_RMID, nullptr);
        return;
    }
    
    if (pid == 0) {
        // 子进程：发送消息
        Logger::info("MSGQ-SENDER", "开始发送消息...");
        
        // 发送不同类型的消息
        MsgBuffer messages[] = {
            {1, "Type 1: Normal message", 1},
            {2, "Type 2: Important message", 2},
            {1, "Type 1: Another normal message", 1},
            {3, "Type 3: Critical alert!", 3},
            {2, "Type 2: Second important message", 2},
            {99, "Type 99: END signal", 0}
        };
        
        for (const auto& msg : messages) {
            Logger::info("MSGQ-SENDER", "发送 [类型=" + std::to_string(msg.msg_type) + 
                        ", 优先级=" + std::to_string(msg.priority) + "]: " + 
                        std::string(msg.msg_text));
            
            if (msgsnd(msgq_id, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
                Logger::error("MSGQ-SENDER", "msgsnd 失败");
            }
            usleep(100000);
        }
        
        Logger::info("MSGQ-SENDER", "所有消息发送完成");
        _exit(0);
        
    } else {
        // 父进程：接收消息
        usleep(200000);  // 等待一些消息进入队列
        
        Logger::info("MSGQ-RECEIVER", "开始接收消息...\n");
        
        // 演示1：按类型接收（先接收所有类型2的消息）
        Logger::info("MSGQ-RECEIVER", "--- 优先接收类型2的消息 ---");
        MsgBuffer msg;
        
        while (msgrcv(msgq_id, &msg, sizeof(msg) - sizeof(long), 2, IPC_NOWAIT) != -1) {
            Logger::info("MSGQ-RECEIVER", "收到 [类型=" + std::to_string(msg.msg_type) + 
                        "]: " + std::string(msg.msg_text));
        }
        
        // 演示2：接收剩余所有消息
        Logger::info("MSGQ-RECEIVER", "\n--- 接收剩余所有消息 ---");
        
        while (true) {
            // 接收任意类型消息（msg_type = 0）
            ssize_t ret = msgrcv(msgq_id, &msg, sizeof(msg) - sizeof(long), 0, 0);
            if (ret == -1) {
                break;
            }
            
            Logger::info("MSGQ-RECEIVER", "收到 [类型=" + std::to_string(msg.msg_type) + 
                        "]: " + std::string(msg.msg_text));
            
            // 检查结束信号
            if (msg.msg_type == 99) {
                Logger::info("MSGQ-RECEIVER", "收到结束信号");
                break;
            }
        }
        
        // 等待子进程
        int status;
        waitpid(pid, &status, 0);
        
        // 删除消息队列
        msgctl(msgq_id, IPC_RMID, nullptr);
        Logger::info("MSGQ", "消息队列已删除");
    }
    
    Logger::info("MSGQ", "消息队列演示完成\n");
}

} // namespace ipc
