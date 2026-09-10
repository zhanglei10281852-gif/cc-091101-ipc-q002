/**
 * 共享内存 (Shared Memory) 演示
 * 
 * 共享内存是最快的 IPC 方式，多个进程可以直接访问同一块内存区域。
 * 特点：
 * - 速度最快，无需数据拷贝
 * - 需要同步机制（如信号量）来避免竞态条件
 * - 适合大数据量、高频率的进程间通信
 */

#include "ipc_demo.h"
#include <atomic>

namespace ipc {

// 共享内存数据结构
struct SharedData {
    std::atomic<int> counter;
    std::atomic<bool> writer_done;
    char message[256];
    int values[10];
};

void demo_shared_memory() {
    Logger::demo("3. 共享内存 (Shared Memory) 演示");
    
    Logger::info("SHM", "共享内存特点：");
    Logger::info("SHM", "  - 最快的 IPC 方式");
    Logger::info("SHM", "  - 多进程直接访问同一内存");
    Logger::info("SHM", "  - 需要同步机制避免竞态\n");
    
    // 创建共享内存
    key_t key = ftok("/tmp", 'S');
    if (key == -1) {
        Logger::error("SHM", "ftok 失败: " + std::string(strerror(errno)));
        return;
    }
    
    int shm_id = shmget(key, sizeof(SharedData), IPC_CREAT | 0666);
    if (shm_id == -1) {
        Logger::error("SHM", "shmget 失败: " + std::string(strerror(errno)));
        return;
    }
    
    Logger::info("SHM", "共享内存创建成功，ID: " + std::to_string(shm_id));
    Logger::info("SHM", "共享内存大小: " + std::to_string(sizeof(SharedData)) + " bytes");
    
    // 附加共享内存
    SharedData* shared = static_cast<SharedData*>(shmat(shm_id, nullptr, 0));
    if (shared == reinterpret_cast<SharedData*>(-1)) {
        Logger::error("SHM", "shmat 失败: " + std::string(strerror(errno)));
        shmctl(shm_id, IPC_RMID, nullptr);
        return;
    }
    
    // 初始化共享数据
    shared->counter.store(0);
    shared->writer_done.store(false);
    memset(shared->message, 0, sizeof(shared->message));
    memset(shared->values, 0, sizeof(shared->values));
    
    pid_t pid = fork();
    
    if (pid == -1) {
        Logger::error("SHM", "fork 失败");
        shmdt(shared);
        shmctl(shm_id, IPC_RMID, nullptr);
        return;
    }
    
    if (pid == 0) {
        // 子进程：写入数据
        Logger::info("SHM-WRITER", "开始写入共享内存...");
        
        // 写入消息
        strcpy(shared->message, "Hello from shared memory!");
        Logger::info("SHM-WRITER", "写入消息: " + std::string(shared->message));
        
        // 写入数组数据
        for (int i = 0; i < 10; i++) {
            shared->values[i] = (i + 1) * 10;
            shared->counter.fetch_add(1);
            Logger::info("SHM-WRITER", "写入 values[" + std::to_string(i) + "] = " + 
                        std::to_string(shared->values[i]));
            usleep(50000);
        }
        
        shared->writer_done.store(true);
        Logger::info("SHM-WRITER", "写入完成，counter = " + 
                    std::to_string(shared->counter.load()));
        
        shmdt(shared);
        _exit(0);
        
    } else {
        // 父进程：读取数据
        Logger::info("SHM-READER", "等待子进程写入...\n");
        
        // 等待写入完成
        while (!shared->writer_done.load()) {
            usleep(100000);
        }
        
        Logger::info("SHM-READER", "读取共享内存数据:");
        Logger::info("SHM-READER", "  消息: " + std::string(shared->message));
        Logger::info("SHM-READER", "  计数器: " + std::to_string(shared->counter.load()));
        
        std::string values_str = "  数组: [";
        for (int i = 0; i < 10; i++) {
            values_str += std::to_string(shared->values[i]);
            if (i < 9) values_str += ", ";
        }
        values_str += "]";
        Logger::info("SHM-READER", values_str);
        
        // 等待子进程
        int status;
        waitpid(pid, &status, 0);
        
        // 分离并删除共享内存
        shmdt(shared);
        shmctl(shm_id, IPC_RMID, nullptr);
        Logger::info("SHM", "共享内存已释放");
    }
    
    Logger::info("SHM", "共享内存演示完成\n");
}

} // namespace ipc
