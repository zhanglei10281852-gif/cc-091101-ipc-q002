/**
 * 信号 (Signal) 测试用例
 */

#include "test_framework.h"

using namespace test;

// 全局变量用于信号处理
static volatile sig_atomic_t g_signal_received = 0;
static volatile sig_atomic_t g_signal_count = 0;

// 信号处理函数
static void test_signal_handler(int signum) {
    g_signal_received = signum;
    g_signal_count++;
}

// 重置信号状态
static void reset_signal_state() {
    g_signal_received = 0;
    g_signal_count = 0;
}

// 测试：注册信号处理函数
void test_signal_register_handler() {
    reset_signal_state();
    
    struct sigaction sa;
    sa.sa_handler = test_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    int ret = sigaction(SIGUSR1, &sa, nullptr);
    ASSERT_EQ(ret, 0);
    
    // 恢复默认处理
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, nullptr);
}

// 测试：发送信号给自己
void test_signal_send_to_self() {
    reset_signal_state();
    
    struct sigaction sa;
    sa.sa_handler = test_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, nullptr);
    
    // 发送信号给自己
    int ret = raise(SIGUSR1);
    ASSERT_EQ(ret, 0);
    
    // 验证信号被接收
    ASSERT_EQ(g_signal_received, SIGUSR1);
    ASSERT_EQ(g_signal_count, 1);
    
    // 恢复
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, nullptr);
}

// 测试：父子进程信号通信
void test_signal_fork_communication() {
    reset_signal_state();
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        // 子进程：设置信号处理并等待
        struct sigaction sa;
        sa.sa_handler = test_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGUSR1, &sa, nullptr);
        sigaction(SIGUSR2, &sa, nullptr);
        
        // 等待信号
        int expected = 3;
        while (g_signal_count < expected) {
            pause();
        }
        
        _exit(g_signal_count == expected ? 0 : 1);
    } else {
        // 父进程：发送信号
        usleep(100000);  // 等待子进程设置信号处理
        
        kill(pid, SIGUSR1);
        usleep(50000);
        kill(pid, SIGUSR2);
        usleep(50000);
        kill(pid, SIGUSR1);
        
        int status;
        waitpid(pid, &status, 0);
        
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 0);
    }
}

// 测试：忽略信号
void test_signal_ignore() {
    reset_signal_state();
    
    // 设置忽略 SIGUSR1
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, nullptr);
    
    // 发送信号（应被忽略）
    raise(SIGUSR1);
    
    // 信号计数应为 0
    ASSERT_EQ(g_signal_count, 0);
    
    // 恢复
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, nullptr);
}

// 测试：多个信号
void test_signal_multiple_signals() {
    reset_signal_state();
    
    struct sigaction sa;
    sa.sa_handler = test_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGUSR1, &sa, nullptr);
    sigaction(SIGUSR2, &sa, nullptr);
    
    // 发送多个信号
    raise(SIGUSR1);
    raise(SIGUSR2);
    raise(SIGUSR1);
    
    ASSERT_EQ(g_signal_count, 3);
    
    // 恢复
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, nullptr);
    sigaction(SIGUSR2, &sa, nullptr);
}

// 测试：使用 kill() 发送信号
void test_signal_kill() {
    reset_signal_state();
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        struct sigaction sa;
        sa.sa_handler = test_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGUSR1, &sa, nullptr);
        
        pause();  // 等待信号
        
        _exit(g_signal_received == SIGUSR1 ? 0 : 1);
    } else {
        usleep(100000);
        
        // 使用 kill 发送信号
        int ret = kill(pid, SIGUSR1);
        ASSERT_EQ(ret, 0);
        
        int status;
        waitpid(pid, &status, 0);
        
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 0);
    }
}

// 测试：信号掩码
void test_signal_mask() {
    reset_signal_state();
    
    struct sigaction sa;
    sa.sa_handler = test_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, nullptr);
    
    // 阻塞 SIGUSR1
    sigset_t block_set, old_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block_set, &old_set);
    
    // 发送信号（应被阻塞）
    raise(SIGUSR1);
    ASSERT_EQ(g_signal_count, 0);  // 信号被阻塞，未处理
    
    // 解除阻塞
    sigprocmask(SIG_UNBLOCK, &block_set, nullptr);
    
    // 信号应该被处理
    ASSERT_EQ(g_signal_count, 1);
    
    // 恢复
    sigprocmask(SIG_SETMASK, &old_set, nullptr);
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, nullptr);
}

// 测试：检查进程是否存在
void test_signal_check_process() {
    // kill(pid, 0) 可以检查进程是否存在
    pid_t my_pid = getpid();
    
    // 自己应该存在
    int ret = kill(my_pid, 0);
    ASSERT_EQ(ret, 0);
    
    // 不存在的进程
    ret = kill(99999, 0);
    ASSERT_EQ(ret, -1);
    ASSERT_EQ(errno, ESRCH);
}

void test_signal_suite() {
    run_test("signal_register_handler", test_signal_register_handler);
    run_test("signal_send_to_self", test_signal_send_to_self);
    run_test("signal_fork_communication", test_signal_fork_communication);
    run_test("signal_ignore", test_signal_ignore);
    run_test("signal_multiple_signals", test_signal_multiple_signals);
    run_test("signal_kill", test_signal_kill);
    run_test("signal_mask", test_signal_mask);
    run_test("signal_check_process", test_signal_check_process);
}
