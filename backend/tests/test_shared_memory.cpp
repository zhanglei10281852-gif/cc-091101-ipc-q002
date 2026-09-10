/**
 * 共享内存 (Shared Memory) 测试用例
 */

#include "test_framework.h"

using namespace test;

// 测试：创建共享内存
void test_shm_create() {
    key_t key = ftok("/tmp", 'T');
    ASSERT_NE(key, -1);
    
    int shm_id = shmget(key, 1024, IPC_CREAT | IPC_EXCL | 0666);
    ASSERT_NE(shm_id, -1);
    
    // 清理
    shmctl(shm_id, IPC_RMID, nullptr);
}

// 测试：附加和分离共享内存
void test_shm_attach_detach() {
    key_t key = ftok("/tmp", 'A');
    ASSERT_NE(key, -1);
    
    int shm_id = shmget(key, 1024, IPC_CREAT | 0666);
    ASSERT_NE(shm_id, -1);
    
    // 附加
    void* ptr = shmat(shm_id, nullptr, 0);
    ASSERT_NE(ptr, (void*)-1);
    
    // 分离
    int ret = shmdt(ptr);
    ASSERT_EQ(ret, 0);
    
    // 清理
    shmctl(shm_id, IPC_RMID, nullptr);
}

// 测试：共享内存读写
void test_shm_read_write() {
    key_t key = ftok("/tmp", 'W');
    ASSERT_NE(key, -1);
    
    int shm_id = shmget(key, 256, IPC_CREAT | 0666);
    ASSERT_NE(shm_id, -1);
    
    char* ptr = static_cast<char*>(shmat(shm_id, nullptr, 0));
    ASSERT_NE(ptr, (char*)-1);
    
    // 写入
    const char* test_str = "Shared Memory Test";
    strcpy(ptr, test_str);
    
    // 读取验证
    ASSERT_STREQ(ptr, test_str);
    
    shmdt(ptr);
    shmctl(shm_id, IPC_RMID, nullptr);
}

// 测试：父子进程共享内存通信
void test_shm_fork_communication() {
    key_t key = ftok("/tmp", 'F');
    ASSERT_NE(key, -1);
    
    struct SharedData {
        std::atomic<int> counter;
        std::atomic<bool> ready;
        char message[64];
    };
    
    int shm_id = shmget(key, sizeof(SharedData), IPC_CREAT | 0666);
    ASSERT_NE(shm_id, -1);
    
    SharedData* shared = static_cast<SharedData*>(shmat(shm_id, nullptr, 0));
    ASSERT_NE(shared, (SharedData*)-1);
    
    // 初始化
    shared->counter.store(0);
    shared->ready.store(false);
    memset(shared->message, 0, sizeof(shared->message));
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        // 子进程：写入数据
        strcpy(shared->message, "Hello from child!");
        shared->counter.store(42);
        shared->ready.store(true);
        
        shmdt(shared);
        _exit(0);
    } else {
        // 父进程：等待并读取
        while (!shared->ready.load()) {
            usleep(10000);
        }
        
        ASSERT_STREQ(shared->message, "Hello from child!");
        ASSERT_EQ(shared->counter.load(), 42);
        
        waitpid(pid, nullptr, 0);
        shmdt(shared);
        shmctl(shm_id, IPC_RMID, nullptr);
    }
}

// 测试：共享内存大小
void test_shm_size() {
    key_t key = ftok("/tmp", 'S');
    ASSERT_NE(key, -1);
    
    size_t requested_size = 4096;
    int shm_id = shmget(key, requested_size, IPC_CREAT | 0666);
    ASSERT_NE(shm_id, -1);
    
    // 获取共享内存信息
    struct shmid_ds info;
    ASSERT_EQ(shmctl(shm_id, IPC_STAT, &info), 0);
    
    // 实际大小应 >= 请求大小
    ASSERT_GE(info.shm_segsz, requested_size);
    
    shmctl(shm_id, IPC_RMID, nullptr);
}

// 测试：原子操作
void test_shm_atomic_operations() {
    key_t key = ftok("/tmp", 'O');
    ASSERT_NE(key, -1);
    
    struct AtomicData {
        std::atomic<int> value;
    };
    
    int shm_id = shmget(key, sizeof(AtomicData), IPC_CREAT | 0666);
    ASSERT_NE(shm_id, -1);
    
    AtomicData* data = static_cast<AtomicData*>(shmat(shm_id, nullptr, 0));
    ASSERT_NE(data, (AtomicData*)-1);
    
    data->value.store(0);
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        // 子进程：递增 100 次
        for (int i = 0; i < 100; i++) {
            data->value.fetch_add(1);
        }
        shmdt(data);
        _exit(0);
    } else {
        // 父进程：也递增 100 次
        for (int i = 0; i < 100; i++) {
            data->value.fetch_add(1);
        }
        
        waitpid(pid, nullptr, 0);
        
        // 总计应为 200
        ASSERT_EQ(data->value.load(), 200);
        
        shmdt(data);
        shmctl(shm_id, IPC_RMID, nullptr);
    }
}

// 测试：数组数据共享
void test_shm_array_data() {
    key_t key = ftok("/tmp", 'R');
    ASSERT_NE(key, -1);
    
    const int ARRAY_SIZE = 10;
    int shm_id = shmget(key, sizeof(int) * ARRAY_SIZE, IPC_CREAT | 0666);
    ASSERT_NE(shm_id, -1);
    
    int* arr = static_cast<int*>(shmat(shm_id, nullptr, 0));
    ASSERT_NE(arr, (int*)-1);
    
    // 写入数据
    for (int i = 0; i < ARRAY_SIZE; i++) {
        arr[i] = i * i;
    }
    
    // 验证
    for (int i = 0; i < ARRAY_SIZE; i++) {
        ASSERT_EQ(arr[i], i * i);
    }
    
    shmdt(arr);
    shmctl(shm_id, IPC_RMID, nullptr);
}

void test_shared_memory_suite() {
    run_test("shm_create", test_shm_create);
    run_test("shm_attach_detach", test_shm_attach_detach);
    run_test("shm_read_write", test_shm_read_write);
    run_test("shm_fork_communication", test_shm_fork_communication);
    run_test("shm_size", test_shm_size);
    run_test("shm_atomic_operations", test_shm_atomic_operations);
    run_test("shm_array_data", test_shm_array_data);
}
