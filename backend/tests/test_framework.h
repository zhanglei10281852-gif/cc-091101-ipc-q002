#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <atomic>

namespace test {

// 测试结果
struct TestResult {
    std::string name;
    bool passed;
    std::string message;
    double duration_ms;
};

// 测试统计
struct TestStats {
    int total = 0;
    int passed = 0;
    int failed = 0;
};

// 全局测试结果
inline std::vector<TestResult> g_results;
inline TestStats g_stats;

// 颜色输出
inline void print_green(const std::string& msg) {
    std::cout << "\033[32m" << msg << "\033[0m";
}

inline void print_red(const std::string& msg) {
    std::cout << "\033[31m" << msg << "\033[0m";
}

inline void print_yellow(const std::string& msg) {
    std::cout << "\033[33m" << msg << "\033[0m";
}

inline void print_cyan(const std::string& msg) {
    std::cout << "\033[36m" << msg << "\033[0m";
}

// 断言宏
#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            throw std::runtime_error("ASSERT_TRUE failed: " #cond); \
        } \
    } while(0)

#define ASSERT_FALSE(cond) \
    do { \
        if (cond) { \
            throw std::runtime_error("ASSERT_FALSE failed: " #cond); \
        } \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            throw std::runtime_error("ASSERT_EQ failed: " #a " != " #b); \
        } \
    } while(0)

#define ASSERT_NE(a, b) \
    do { \
        if ((a) == (b)) { \
            throw std::runtime_error("ASSERT_NE failed: " #a " == " #b); \
        } \
    } while(0)

#define ASSERT_GT(a, b) \
    do { \
        if (!((a) > (b))) { \
            throw std::runtime_error("ASSERT_GT failed: " #a " <= " #b); \
        } \
    } while(0)

#define ASSERT_GE(a, b) \
    do { \
        if (!((a) >= (b))) { \
            throw std::runtime_error("ASSERT_GE failed: " #a " < " #b); \
        } \
    } while(0)

#define ASSERT_STREQ(a, b) \
    do { \
        if (strcmp((a), (b)) != 0) { \
            throw std::runtime_error("ASSERT_STREQ failed: strings not equal"); \
        } \
    } while(0)

// 运行单个测试
inline void run_test(const std::string& name, std::function<void()> test_func) {
    std::cout << "  Running: " << name << " ... ";
    std::cout.flush();
    
    auto start = std::chrono::high_resolution_clock::now();
    TestResult result;
    result.name = name;
    
    try {
        test_func();
        result.passed = true;
        result.message = "OK";
        print_green("PASSED");
    } catch (const std::exception& e) {
        result.passed = false;
        result.message = e.what();
        print_red("FAILED");
        std::cout << " (" << e.what() << ")";
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    result.duration_ms = std::chrono::duration<double, std::milli>(end - start).count();
    
    std::cout << " [" << result.duration_ms << " ms]" << std::endl;
    
    g_results.push_back(result);
    g_stats.total++;
    if (result.passed) {
        g_stats.passed++;
    } else {
        g_stats.failed++;
    }
}

// 测试套件
inline void run_suite(const std::string& name, std::function<void()> suite_func) {
    std::cout << std::endl;
    print_cyan("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    print_cyan("  Test Suite: " + name + "\n");
    print_cyan("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    suite_func();
}

// 打印最终结果
inline int print_summary() {
    std::cout << std::endl;
    print_cyan("══════════════════════════════════════════════════\n");
    print_cyan("                  TEST SUMMARY\n");
    print_cyan("══════════════════════════════════════════════════\n");
    
    std::cout << "  Total:  " << g_stats.total << std::endl;
    std::cout << "  ";
    print_green("Passed: " + std::to_string(g_stats.passed));
    std::cout << std::endl;
    
    if (g_stats.failed > 0) {
        std::cout << "  ";
        print_red("Failed: " + std::to_string(g_stats.failed));
        std::cout << std::endl;
        
        std::cout << std::endl;
        print_red("  Failed tests:\n");
        for (const auto& r : g_results) {
            if (!r.passed) {
                std::cout << "    - " << r.name << ": " << r.message << std::endl;
            }
        }
    }
    
    std::cout << std::endl;
    if (g_stats.failed == 0) {
        print_green("  ✓ All tests passed!\n");
    } else {
        print_red("  ✗ Some tests failed!\n");
    }
    print_cyan("══════════════════════════════════════════════════\n");
    
    return g_stats.failed > 0 ? 1 : 0;
}

} // namespace test

#endif // TEST_FRAMEWORK_H
