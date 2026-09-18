# IPC Demo Backend

C++ 进程间通信演示程序。

## 本地编译

```bash
mkdir -p build && cd build
cmake ..
make
./ipc_demo
```

## 运行选项

- 交互模式：`./ipc_demo`
- 运行所有演示：`./ipc_demo --all`
- 本地代理服务端：`./ipc_demo serve [选项]`
- 本地代理客户端：`./ipc_demo request [选项]`

## 本地代理模式 (serve / request)

在原有 Socket 演示之外，`ipc_demo` 内置了可分别启动的本地代理工作模式，
可将多个插件进程的请求汇入一个服务端进程：

```bash
# 终端 1: 启动服务端 (可同时维护多个客户端)
./ipc_demo serve --path /tmp/ipc_proxy.sock --max-frame 1048576 --idle-timeout 60 --grace 5

# 终端 2/3/...: 启动任意多个客户端
./ipc_demo request --path /tmp/ipc_proxy.sock --count 5
./ipc_demo request --path /tmp/ipc_proxy.sock --pipeline --count 5   # 一次写入多帧 (粘包)
./ipc_demo request --path /tmp/ipc_proxy.sock --chunk-bytes 1        # 逐字节写入 (拆包)
./ipc_demo request --path /tmp/ipc_proxy.sock --sleep-ms 800 --count 1  # 慢请求

# 优雅退出: 停止接收新连接，在途请求在宽限期内完成后清理 Socket 文件
kill -TERM <serve_pid>
```

### 帧协议

固定 12 字节头部，多字节字段一律大端序：

| 偏移 | 长度 | 含义                              |
| ---- | ---- | --------------------------------- |
| 0    | 2    | magic `'I' 'P'`                   |
| 2    | 1    | 协议版本 (1)                      |
| 3    | 1    | 类型 (1=request, 2=response)      |
| 4    | 4    | request_id (uint32, 大端)         |
| 8    | 4    | payload_len (uint32, 大端)        |
| 12   | N    | payload                           |

同一连接内响应严格按请求顺序返回；慢连接/慢消费者不会阻塞其他连接。
请求 payload 为 `SLEEP:<ms>:<text>` 时服务端延迟 ms 后回复 `OK:<text>`，
其余内容立即回复 `ECHO:<payload>`。

### 服务端行为

- 路径被占用时，仅当确认是失效 Socket (connect 两次均 ECONNREFUSED) 才移除；
  普通文件或活跃服务占用会拒绝启动 (退出码 2)
- Socket 文件创建后权限为 0600 (仅当前用户可读写)
- 帧长超过 `--max-frame`、连接空闲超过 `--idle-timeout`、协议字段非法时，
  关闭对应连接并记录可区分的原因 (`reason=frame-too-large` / `idle-timeout` /
  `protocol-error` / `client-disconnect` / `server-shutdown` 等)
- SIGTERM 后停止接收新连接，在途请求在 `--grace` 宽限期内完成再清理 Socket
  文件 (退出码 0)；超期则以退出码 3 明确失败

### 退出码

serve: `0`=优雅退出 `1`=一般错误 `2`=路径被占用 `3`=宽限期超期
request: `0`=成功 `2`=连接失败 `3`=I/O 错误 `4`=被服务端关闭 `5`=响应校验失败 `6`=超时

完整选项见 `./ipc_demo serve --help` 与 `./ipc_demo request --help`。

## 测试

```bash
mkdir -p build && cd build
cmake -DBUILD_TESTS=ON ..
make
./tests/ipc_tests        # 或直接 ctest
```

`tests/test_socket_proxy.cpp` 是 Linux 集成测试：以独立进程启动真正的
`ipc_demo serve` 服务端与多个 `ipc_demo request` 客户端，通过退出码与日志
输出验证并发响应、拆包/粘包、超限拒绝、空闲超时、协议错误、失效 Socket
清理、0600 权限以及 SIGTERM 优雅退出/宽限期超期。

## 依赖

- GCC 9+ 或 Clang 10+
- CMake 3.16+
- POSIX 兼容系统 (Linux/macOS)
