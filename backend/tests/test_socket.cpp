/**
 * Unix Domain Socket 测试用例
 */

#include "test_framework.h"

using namespace test;

static const char* TEST_SOCKET_PATH = "/tmp/test_socket";

// 清理 socket 文件
static void cleanup_socket() {
    unlink(TEST_SOCKET_PATH);
}

// 测试：创建 socket
void test_socket_create() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_NE(fd, -1);
    close(fd);
    
    // 测试 DGRAM 类型
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT_NE(fd, -1);
    close(fd);
}

// 测试：绑定 socket
void test_socket_bind() {
    cleanup_socket();
    
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_NE(fd, -1);
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, TEST_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    int ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    ASSERT_EQ(ret, 0);
    
    // 验证 socket 文件存在
    struct stat st;
    ASSERT_EQ(stat(TEST_SOCKET_PATH, &st), 0);
    ASSERT_TRUE(S_ISSOCK(st.st_mode));
    
    close(fd);
    cleanup_socket();
}

// 测试：监听
void test_socket_listen() {
    cleanup_socket();
    
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_NE(fd, -1);
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, TEST_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    ASSERT_EQ(bind(fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
    
    int ret = listen(fd, 5);
    ASSERT_EQ(ret, 0);
    
    close(fd);
    cleanup_socket();
}

// 测试：客户端-服务器通信
void test_socket_client_server() {
    cleanup_socket();
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        // 子进程：客户端
        usleep(100000);  // 等待服务器启动
        
        int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client_fd == -1) _exit(1);
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, TEST_SOCKET_PATH, sizeof(addr.sun_path) - 1);
        
        if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            close(client_fd);
            _exit(2);
        }
        
        // 发送消息
        const char* msg = "Hello Server!";
        send(client_fd, msg, strlen(msg) + 1, 0);
        
        // 接收响应
        char buffer[64];
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);
        
        close(client_fd);
        
        if (bytes > 0 && strcmp(buffer, "Hello Client!") == 0) {
            _exit(0);
        }
        _exit(3);
        
    } else {
        // 父进程：服务器
        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT_NE(server_fd, -1);
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, TEST_SOCKET_PATH, sizeof(addr.sun_path) - 1);
        
        ASSERT_EQ(bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
        ASSERT_EQ(listen(server_fd, 5), 0);
        
        // 接受连接
        int client_fd = accept(server_fd, nullptr, nullptr);
        ASSERT_NE(client_fd, -1);
        
        // 接收消息
        char buffer[64];
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);
        ASSERT_GT(bytes, 0);
        ASSERT_STREQ(buffer, "Hello Server!");
        
        // 发送响应
        const char* response = "Hello Client!";
        send(client_fd, response, strlen(response) + 1, 0);
        
        close(client_fd);
        close(server_fd);
        
        int status;
        waitpid(pid, &status, 0);
        
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 0);
        
        cleanup_socket();
    }
}

// 测试：多条消息
void test_socket_multiple_messages() {
    cleanup_socket();
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        usleep(100000);
        
        int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client_fd == -1) _exit(1);
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, TEST_SOCKET_PATH, sizeof(addr.sun_path) - 1);
        
        if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            close(client_fd);
            _exit(2);
        }
        
        // 发送多条消息
        for (int i = 1; i <= 5; i++) {
            char msg[32];
            snprintf(msg, sizeof(msg), "MSG_%d", i);
            send(client_fd, msg, strlen(msg) + 1, 0);
            usleep(10000);
        }
        
        close(client_fd);
        _exit(0);
        
    } else {
        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT_NE(server_fd, -1);
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, TEST_SOCKET_PATH, sizeof(addr.sun_path) - 1);
        
        ASSERT_EQ(bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
        ASSERT_EQ(listen(server_fd, 5), 0);
        
        int client_fd = accept(server_fd, nullptr, nullptr);
        ASSERT_NE(client_fd, -1);
        
        char buffer[32];
        int count = 0;
        
        while (recv(client_fd, buffer, sizeof(buffer), 0) > 0) {
            count++;
            char expected[32];
            snprintf(expected, sizeof(expected), "MSG_%d", count);
            ASSERT_STREQ(buffer, expected);
        }
        
        ASSERT_EQ(count, 5);
        
        close(client_fd);
        close(server_fd);
        waitpid(pid, nullptr, 0);
        cleanup_socket();
    }
}

// 测试：DGRAM socket
void test_socket_dgram() {
    const char* dgram_path = "/tmp/test_dgram_socket";
    unlink(dgram_path);
    
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT_NE(fd, -1);
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, dgram_path, sizeof(addr.sun_path) - 1);
    
    ASSERT_EQ(bind(fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
    
    close(fd);
    unlink(dgram_path);
}

// 测试：非阻塞 socket
void test_socket_nonblocking() {
    cleanup_socket();
    
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    ASSERT_NE(fd, -1);
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, TEST_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    ASSERT_EQ(bind(fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
    ASSERT_EQ(listen(fd, 5), 0);
    
    // 非阻塞 accept 应返回 -1，errno = EAGAIN
    int client_fd = accept(fd, nullptr, nullptr);
    ASSERT_EQ(client_fd, -1);
    ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    
    close(fd);
    cleanup_socket();
}

// 测试：双向通信
void test_socket_bidirectional() {
    cleanup_socket();
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        usleep(100000);
        
        int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client_fd == -1) _exit(1);
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, TEST_SOCKET_PATH, sizeof(addr.sun_path) - 1);
        
        if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            close(client_fd);
            _exit(2);
        }
        
        // 双向通信：发送-接收-发送-接收
        char buffer[64];
        
        send(client_fd, "PING", 5, 0);
        recv(client_fd, buffer, sizeof(buffer), 0);
        if (strcmp(buffer, "PONG") != 0) _exit(3);
        
        send(client_fd, "DATA", 5, 0);
        recv(client_fd, buffer, sizeof(buffer), 0);
        if (strcmp(buffer, "ACK") != 0) _exit(4);
        
        close(client_fd);
        _exit(0);
        
    } else {
        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT_NE(server_fd, -1);
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, TEST_SOCKET_PATH, sizeof(addr.sun_path) - 1);
        
        ASSERT_EQ(bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
        ASSERT_EQ(listen(server_fd, 5), 0);
        
        int client_fd = accept(server_fd, nullptr, nullptr);
        ASSERT_NE(client_fd, -1);
        
        char buffer[64];
        
        // 接收 PING，发送 PONG
        recv(client_fd, buffer, sizeof(buffer), 0);
        ASSERT_STREQ(buffer, "PING");
        send(client_fd, "PONG", 5, 0);
        
        // 接收 DATA，发送 ACK
        recv(client_fd, buffer, sizeof(buffer), 0);
        ASSERT_STREQ(buffer, "DATA");
        send(client_fd, "ACK", 4, 0);
        
        close(client_fd);
        close(server_fd);
        
        int status;
        waitpid(pid, &status, 0);
        
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 0);
        
        cleanup_socket();
    }
}

void test_socket_suite() {
    run_test("socket_create", test_socket_create);
    run_test("socket_bind", test_socket_bind);
    run_test("socket_listen", test_socket_listen);
    run_test("socket_client_server", test_socket_client_server);
    run_test("socket_multiple_messages", test_socket_multiple_messages);
    run_test("socket_dgram", test_socket_dgram);
    run_test("socket_nonblocking", test_socket_nonblocking);
    run_test("socket_bidirectional", test_socket_bidirectional);
}
