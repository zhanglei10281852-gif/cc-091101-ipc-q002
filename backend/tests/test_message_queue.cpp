/**
 * 消息队列 (Message Queue) 测试用例
 */

#include "test_framework.h"

using namespace test;

// 消息结构
struct TestMsg {
    long mtype;
    char mtext[64];
};

// 测试：创建消息队列
void test_msgq_create() {
    key_t key = ftok("/tmp", 'Q');
    ASSERT_NE(key, -1);
    
    int msgq_id = msgget(key, IPC_CREAT | IPC_EXCL | 0666);
    ASSERT_NE(msgq_id, -1);
    
    // 清理
    msgctl(msgq_id, IPC_RMID, nullptr);
}

// 测试：发送和接收消息
void test_msgq_send_receive() {
    key_t key = ftok("/tmp", 'R');
    ASSERT_NE(key, -1);
    
    int msgq_id = msgget(key, IPC_CREAT | 0666);
    ASSERT_NE(msgq_id, -1);
    
    // 发送消息
    TestMsg send_msg;
    send_msg.mtype = 1;
    strcpy(send_msg.mtext, "Test Message");
    
    int ret = msgsnd(msgq_id, &send_msg, sizeof(send_msg.mtext), 0);
    ASSERT_EQ(ret, 0);
    
    // 接收消息
    TestMsg recv_msg;
    ssize_t bytes = msgrcv(msgq_id, &recv_msg, sizeof(recv_msg.mtext), 1, 0);
    ASSERT_GT(bytes, 0);
    ASSERT_EQ(recv_msg.mtype, 1);
    ASSERT_STREQ(recv_msg.mtext, "Test Message");
    
    msgctl(msgq_id, IPC_RMID, nullptr);
}

// 测试：消息类型过滤
void test_msgq_type_filter() {
    key_t key = ftok("/tmp", 'F');
    ASSERT_NE(key, -1);
    
    int msgq_id = msgget(key, IPC_CREAT | 0666);
    ASSERT_NE(msgq_id, -1);
    
    // 发送不同类型的消息
    TestMsg msg1 = {1, "Type 1 Message"};
    TestMsg msg2 = {2, "Type 2 Message"};
    TestMsg msg3 = {1, "Another Type 1"};
    
    msgsnd(msgq_id, &msg1, sizeof(msg1.mtext), 0);
    msgsnd(msgq_id, &msg2, sizeof(msg2.mtext), 0);
    msgsnd(msgq_id, &msg3, sizeof(msg3.mtext), 0);
    
    // 只接收类型 2 的消息
    TestMsg recv;
    ssize_t bytes = msgrcv(msgq_id, &recv, sizeof(recv.mtext), 2, 0);
    ASSERT_GT(bytes, 0);
    ASSERT_EQ(recv.mtype, 2);
    ASSERT_STREQ(recv.mtext, "Type 2 Message");
    
    // 接收类型 1 的消息（应该是第一条）
    bytes = msgrcv(msgq_id, &recv, sizeof(recv.mtext), 1, 0);
    ASSERT_GT(bytes, 0);
    ASSERT_STREQ(recv.mtext, "Type 1 Message");
    
    // 再接收类型 1（应该是第三条）
    bytes = msgrcv(msgq_id, &recv, sizeof(recv.mtext), 1, 0);
    ASSERT_GT(bytes, 0);
    ASSERT_STREQ(recv.mtext, "Another Type 1");
    
    msgctl(msgq_id, IPC_RMID, nullptr);
}

// 测试：非阻塞接收
void test_msgq_nonblocking() {
    key_t key = ftok("/tmp", 'N');
    ASSERT_NE(key, -1);
    
    int msgq_id = msgget(key, IPC_CREAT | 0666);
    ASSERT_NE(msgq_id, -1);
    
    // 队列为空时非阻塞接收
    TestMsg recv;
    ssize_t bytes = msgrcv(msgq_id, &recv, sizeof(recv.mtext), 0, IPC_NOWAIT);
    ASSERT_EQ(bytes, -1);
    ASSERT_EQ(errno, ENOMSG);
    
    msgctl(msgq_id, IPC_RMID, nullptr);
}

// 测试：父子进程通信
void test_msgq_fork_communication() {
    key_t key = ftok("/tmp", 'C');
    ASSERT_NE(key, -1);
    
    int msgq_id = msgget(key, IPC_CREAT | 0666);
    ASSERT_NE(msgq_id, -1);
    
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    
    if (pid == 0) {
        // 子进程：发送消息
        for (int i = 1; i <= 3; i++) {
            TestMsg msg;
            msg.mtype = i;
            snprintf(msg.mtext, sizeof(msg.mtext), "Message %d", i);
            msgsnd(msgq_id, &msg, sizeof(msg.mtext), 0);
            usleep(10000);
        }
        _exit(0);
    } else {
        // 父进程：接收消息
        usleep(50000);  // 等待子进程发送
        
        TestMsg recv;
        int count = 0;
        
        for (int i = 1; i <= 3; i++) {
            ssize_t bytes = msgrcv(msgq_id, &recv, sizeof(recv.mtext), i, 0);
            if (bytes > 0) {
                count++;
                char expected[64];
                snprintf(expected, sizeof(expected), "Message %d", i);
                ASSERT_STREQ(recv.mtext, expected);
            }
        }
        
        waitpid(pid, nullptr, 0);
        ASSERT_EQ(count, 3);
        
        msgctl(msgq_id, IPC_RMID, nullptr);
    }
}

// 测试：消息队列状态
void test_msgq_status() {
    key_t key = ftok("/tmp", 'S');
    ASSERT_NE(key, -1);
    
    int msgq_id = msgget(key, IPC_CREAT | 0666);
    ASSERT_NE(msgq_id, -1);
    
    // 发送几条消息
    TestMsg msg = {1, "Test"};
    msgsnd(msgq_id, &msg, sizeof(msg.mtext), 0);
    msgsnd(msgq_id, &msg, sizeof(msg.mtext), 0);
    
    // 获取队列状态
    struct msqid_ds info;
    ASSERT_EQ(msgctl(msgq_id, IPC_STAT, &info), 0);
    
    // 应该有 2 条消息
    ASSERT_EQ(info.msg_qnum, 2);
    
    msgctl(msgq_id, IPC_RMID, nullptr);
}

// 测试：接收任意类型消息
void test_msgq_receive_any_type() {
    key_t key = ftok("/tmp", 'A');
    ASSERT_NE(key, -1);
    
    int msgq_id = msgget(key, IPC_CREAT | 0666);
    ASSERT_NE(msgq_id, -1);
    
    // 发送不同类型
    TestMsg msg1 = {5, "Type 5"};
    TestMsg msg2 = {3, "Type 3"};
    TestMsg msg3 = {7, "Type 7"};
    
    msgsnd(msgq_id, &msg1, sizeof(msg1.mtext), 0);
    msgsnd(msgq_id, &msg2, sizeof(msg2.mtext), 0);
    msgsnd(msgq_id, &msg3, sizeof(msg3.mtext), 0);
    
    // 接收任意类型（mtype=0），应按 FIFO 顺序
    TestMsg recv;
    
    msgrcv(msgq_id, &recv, sizeof(recv.mtext), 0, 0);
    ASSERT_EQ(recv.mtype, 5);
    
    msgrcv(msgq_id, &recv, sizeof(recv.mtext), 0, 0);
    ASSERT_EQ(recv.mtype, 3);
    
    msgrcv(msgq_id, &recv, sizeof(recv.mtext), 0, 0);
    ASSERT_EQ(recv.mtype, 7);
    
    msgctl(msgq_id, IPC_RMID, nullptr);
}

void test_message_queue_suite() {
    run_test("msgq_create", test_msgq_create);
    run_test("msgq_send_receive", test_msgq_send_receive);
    run_test("msgq_type_filter", test_msgq_type_filter);
    run_test("msgq_nonblocking", test_msgq_nonblocking);
    run_test("msgq_fork_communication", test_msgq_fork_communication);
    run_test("msgq_status", test_msgq_status);
    run_test("msgq_receive_any_type", test_msgq_receive_any_type);
}
