/**
 * 命名管道 (Named Pipe / FIFO) 测试用例
 */

#include "test_framework.h"

using namespace test;

static const char* TEST_FIFO_PATH = "/tmp/test_fifo";

// 清理 FIFO
static void cleanup_fifo() {
    unlink(TEST_FIFO_PATH);
}

// 测试：创建命名管道
void test_fifo_create() {
    cleanup_fifo();
    
    int ret = mkfifo(TEST_FIFO_PATH, 0666);
    ASSERT_EQ(ret, 0);
    
    // 验证文件存在且是 FIFO
    struct stat st;
    ASSERT_EQ(stat(TEST_FIFO_PATH, &st), 0);
    ASSERT_TRUE(S_ISFIFO(st.st_mode));
    
    cleanup_fifo();
}

// 测试：重复创建应失败
void test_fifo_create_duplicate() {
    cleanup_fifo();
    
    ASSERT_EQ(mkfifo(TEST_FIFO_PATH, 0666), 0);
    
    // 重复创建应返回 -1，errno = EEXIST
    int ret = mkfifo(TEST_FIFO_PATH, 0666);
    ASSERT_EQ(ret, -1);
    ASSERT_EQ(errno, EEXIST);
    
    cleanup_fifo();
}

// 测试：FIFO 读写
void test_fifo_read_write() {
    cleanup_fifo();
    ASSERT_EQ(mkfifo(TEST_FIFO_PATH, 0666), 0);
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        // 子进程：写入
        usleep(50000);  // 等待父进程打开读端
        
        int fd = open(TEST_FIFO_PATH, O_WRONLY);
        if (fd == -1) _exit(1);
        
        const char* msg = "FIFO Test Message";
        write(fd, msg, strlen(msg) + 1);
        close(fd);
        _exit(0);
    } else {
        // 父进程：读取
        int fd = open(TEST_FIFO_PATH, O_RDONLY);
        ASSERT_NE(fd, -1);
        
        char buffer[64] = {0};
        ssize_t bytes = read(fd, buffer, sizeof(buffer));
        close(fd);
        
        int status;
        waitpid(pid, &status, 0);
        
        ASSERT_GT(bytes, 0);
        ASSERT_STREQ(buffer, "FIFO Test Message");
        
        cleanup_fifo();
    }
}

// 测试：结构化数据传输
void test_fifo_struct_transfer() {
    cleanup_fifo();
    ASSERT_EQ(mkfifo(TEST_FIFO_PATH, 0666), 0);
    
    struct TestData {
        int id;
        double value;
        char name[32];
    };
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        usleep(50000);
        
        int fd = open(TEST_FIFO_PATH, O_WRONLY);
        if (fd == -1) _exit(1);
        
        TestData data = {42, 3.14159, "TestStruct"};
        write(fd, &data, sizeof(data));
        close(fd);
        _exit(0);
    } else {
        int fd = open(TEST_FIFO_PATH, O_RDONLY);
        ASSERT_NE(fd, -1);
        
        TestData received;
        ssize_t bytes = read(fd, &received, sizeof(received));
        close(fd);
        
        waitpid(pid, nullptr, 0);
        
        ASSERT_EQ(bytes, sizeof(TestData));
        ASSERT_EQ(received.id, 42);
        ASSERT_TRUE(received.value > 3.14 && received.value < 3.15);
        ASSERT_STREQ(received.name, "TestStruct");
        
        cleanup_fifo();
    }
}

// 测试：非阻塞模式
void test_fifo_nonblocking() {
    cleanup_fifo();
    ASSERT_EQ(mkfifo(TEST_FIFO_PATH, 0666), 0);
    
    // 非阻塞打开读端（无写端时应成功打开）
    int fd = open(TEST_FIFO_PATH, O_RDONLY | O_NONBLOCK);
    ASSERT_NE(fd, -1);
    
    // 读取应返回 0 (EOF) 或 -1 (EAGAIN)，因为没有写端
    char buffer[32];
    ssize_t bytes = read(fd, buffer, sizeof(buffer));
    // 在 Linux 上，非阻塞读取空 FIFO 返回 0 或 -1
    ASSERT_TRUE(bytes == 0 || (bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)));
    
    close(fd);
    cleanup_fifo();
}

// 测试：多条消息
void test_fifo_multiple_messages() {
    cleanup_fifo();
    ASSERT_EQ(mkfifo(TEST_FIFO_PATH, 0666), 0);
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        usleep(50000);
        
        int fd = open(TEST_FIFO_PATH, O_WRONLY);
        if (fd == -1) _exit(1);
        
        for (int i = 0; i < 3; i++) {
            int value = (i + 1) * 100;
            write(fd, &value, sizeof(value));
        }
        
        close(fd);
        _exit(0);
    } else {
        int fd = open(TEST_FIFO_PATH, O_RDONLY);
        ASSERT_NE(fd, -1);
        
        int values[3];
        for (int i = 0; i < 3; i++) {
            read(fd, &values[i], sizeof(int));
        }
        
        close(fd);
        waitpid(pid, nullptr, 0);
        
        ASSERT_EQ(values[0], 100);
        ASSERT_EQ(values[1], 200);
        ASSERT_EQ(values[2], 300);
        
        cleanup_fifo();
    }
}

void test_named_pipe_suite() {
    run_test("fifo_create", test_fifo_create);
    run_test("fifo_create_duplicate", test_fifo_create_duplicate);
    run_test("fifo_read_write", test_fifo_read_write);
    run_test("fifo_struct_transfer", test_fifo_struct_transfer);
    run_test("fifo_nonblocking", test_fifo_nonblocking);
    run_test("fifo_multiple_messages", test_fifo_multiple_messages);
}
