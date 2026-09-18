cmake_minimum_required(VERSION 3.16)
project(ipc_demo VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 查找线程库
find_package(Threads REQUIRED)

# 源文件
set(SOURCES
    src/main.cpp
    src/ipc/pipe_demo.cpp
    src/ipc/named_pipe_demo.cpp
    src/ipc/shared_memory_demo.cpp
    src/ipc/message_queue_demo.cpp
    src/ipc/signal_demo.cpp
    src/ipc/socket_demo.cpp
)

# 可执行文件
add_executable(ipc_demo ${SOURCES})

# 包含目录
target_include_directories(ipc_demo PRIVATE ${CMAKE_SOURCE_DIR}/src/include)

# 链接库
target_link_libraries(ipc_demo PRIVATE Threads::Threads rt)

# Unix Domain Socket 多客户端代理（serve / request 两种工作模式）
add_executable(ipc_agent
    src/main_agent.cpp
    src/ipc/unix_socket_agent.cpp
)
target_include_directories(ipc_agent PRIVATE ${CMAKE_SOURCE_DIR}/src/include)
target_link_libraries(ipc_agent PRIVATE Threads::Threads)

# 安装
install(TARGETS ipc_demo ipc_agent DESTINATION bin)

# 添加测试子目录
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
