/**
 * 管道 (Pipe) 测试用例
 */

#include "test_framework.h"

using namespace test;

// 测试：创建管道
void test_pipe_create() {
    int pipefd[2];
    int ret = pipe(pipefd);
    ASSERT_EQ(ret, 0);
    
    // 验证文件描述符有效
    ASSERT_GE(pipefd[0], 0);
    ASSERT_GE(pipefd[1], 0);
    ASSERT_NE(pipefd[0], pipefd[1]);
    
    close(pipefd[0]);
    close(pipefd[1]);
}

// 测试：管道读写
void test_pipe_read_write() {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    
    const char* test_msg = "Hello Pipe!";
    char buffer[64] = {0};
    
    // 写入
    ssize_t written = write(pipefd[1], test_msg, strlen(test_msg) + 1);
    ASSERT_GT(written, 0);
    
    // 读取
    ssize_t read_bytes = read(pipefd[0], buffer, sizeof(buffer));
    ASSERT_GT(read_bytes, 0);
    ASSERT_STREQ(buffer, test_msg);
    
    close(pipefd[0]);
    close(pipefd[1]);
}

// 测试：父子进程通信
void test_pipe_fork_communication() {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        // 子进程：写入
        close(pipefd[0]);
        const char* msg = "Message from child";
        write(pipefd[1], msg, strlen(msg) + 1);
        close(pipefd[1]);
        _exit(0);
    } else {
        // 父进程：读取
        close(pipefd[1]);
        char buffer[64] = {0};
        ssize_t bytes = read(pipefd[0], buffer, sizeof(buffer));
        close(pipefd[0]);
        
        int status;
        waitpid(pid, &status, 0);
        
        ASSERT_GT(bytes, 0);
        ASSERT_STREQ(buffer, "Message from child");
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 0);
    }
}

// 测试：多条消息传输
void test_pipe_multiple_messages() {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        close(pipefd[0]);
        
        for (int i = 1; i <= 5; i++) {
            char msg[32];
            snprintf(msg, sizeof(msg), "MSG_%d", i);
            write(pipefd[1], msg, strlen(msg) + 1);
            usleep(10000);
        }
        
        close(pipefd[1]);
        _exit(0);
    } else {
        close(pipefd[1]);
        
        char buffer[32];
        int count = 0;
        
        while (read(pipefd[0], buffer, sizeof(buffer)) > 0) {
            count++;
            char expected[32];
            snprintf(expected, sizeof(expected), "MSG_%d", count);
            ASSERT_STREQ(buffer, expected);
        }
        
        close(pipefd[0]);
        waitpid(pid, nullptr, 0);
        
        ASSERT_EQ(count, 5);
    }
}

// 测试：关闭写端后读取返回 0
void test_pipe_close_write_end() {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    
    // 写入一条消息
    const char* msg = "test";
    write(pipefd[1], msg, strlen(msg) + 1);
    
    // 关闭写端
    close(pipefd[1]);
    
    // 读取消息
    char buffer[32];
    ssize_t bytes = read(pipefd[0], buffer, sizeof(buffer));
    ASSERT_GT(bytes, 0);
    
    // 再次读取应返回 0 (EOF)
    bytes = read(pipefd[0], buffer, sizeof(buffer));
    ASSERT_EQ(bytes, 0);
    
    close(pipefd[0]);
}

void test_pipe_suite() {
    run_test("pipe_create", test_pipe_create);
    run_test("pipe_read_write", test_pipe_read_write);
    run_test("pipe_fork_communication", test_pipe_fork_communication);
    run_test("pipe_multiple_messages", test_pipe_multiple_messages);
    run_test("pipe_close_write_end", test_pipe_close_write_end);
}
