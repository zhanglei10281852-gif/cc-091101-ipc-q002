cmake_minimum_required(VERSION 3.16)

# 启用测试
enable_testing()

# 查找线程库
find_package(Threads REQUIRED)

# 测试源文件
set(TEST_SOURCES
    test_main.cpp
    test_pipe.cpp
    test_named_pipe.cpp
    test_shared_memory.cpp
    test_message_queue.cpp
    test_signal.cpp
    test_socket.cpp
)

# 测试可执行文件
add_executable(ipc_tests ${TEST_SOURCES})

# 包含目录
target_include_directories(ipc_tests PRIVATE 
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 链接库
target_link_libraries(ipc_tests PRIVATE Threads::Threads rt)

# 添加测试
add_test(NAME ipc_tests COMMAND ipc_tests)
