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

## Unix Domain Socket 代理（`ipc_agent`）

`ipc_demo` 中的 Socket 演示（菜单 6）保留不变；`ipc_agent` 是面向真实场景的
独立程序，提供 `serve` 与 `request` 两个可分别启动的工作模式，用于把多个插件
进程的请求汇入一个本地代理。

### 帧协议

所有多字节整数使用固定大端序（网络字节序）：

| 偏移 | 长度 | 字段           | 说明                                   |
| ---- | ---- | -------------- | -------------------------------------- |
| 0    | 4    | frame_length   | 整帧字节数（含本字段），最小 16        |
| 4    | 8    | request_id     | 请求标识，由客户端分配                 |
| 12   | 1    | type           | 1=REQUEST，2=RESPONSE，3=ERROR         |
| 13   | 1    | flags          | 保留，合法值 0                         |
| 14   | 2    | reserved       | 保留，合法值 0                         |
| 16   | N    | payload        | 长度 = frame_length - 16               |

读取端维护重组缓冲，正确处理**拆包**（一帧分多次到达）、**粘包**
（一次收到多帧）、**短写**（send 部分写入后重试）与 EINTR。

### 启动服务端

```bash
./ipc_agent serve \
    --path /tmp/ipc_agent.sock \
    --max-frame 1048576 \
    --idle-timeout 30000 \
    --grace 5000
```

- 每条连接由独立工作线程处理，单连接内严格按请求顺序返回；某个慢连接
  （睡眠、客户端迟迟不发）只阻塞它自己的线程，不影响其它连接。
- 路径已被占用时，只在确认该节点是**失效 Socket**（无监听者，connect 返回
  ECONNREFUSED）时才移除；仍在服役或不是 Socket 节点则拒绝启动。
- 创建后节点权限收紧为 `0600`（仅当前用户可读写）。
- 帧长超过 `--max-frame`、空闲超过 `--idle-timeout`、协议字段非法都会关闭
  对应连接，并在日志中以不同 `reason=` 记录（`frame-too-large`、
  `idle-timeout`、`bad-type`、`bad-reserved`、`bad-flags`、
  `frame-length-invalid`、`client-aborted`、`peer-closed`、`io-error`）。
- 收到 SIGTERM/SIGINT 后停止接收新连接，已接收的请求允许在 `--grace`
  宽限期内处理完毕并写回，然后删除 Socket 文件正常退出（退出码 0）；
  宽限期到期仍有连接未结束则强制关闭、删除 Socket 文件并以**退出码 2**
  明确失败。

### 启动客户端

```bash
./ipc_agent request --path /tmp/ipc_agent.sock alpha beta "SLEEP:200:slow"
# stdout:
# RESPONSE 1 ACK:alpha
# RESPONSE 2 ACK:beta
# RESPONSE 3 ACK:slow
```

- 消息按顺序发送，响应也严格按 `request_id` 顺序校验；结果行
  （`RESPONSE`/`ERROR <id> <payload>`）写到 stdout，日志写到 stderr。
- 以 `SLEEP:<毫秒>:<内容>` 开头的请求会先睡眠再响应，用于制造慢连接。
- `--byte-at-a-time [--delay-us US]` 逐字节发送，制造半包输入。
- `--send-raw-hex HEX` 原样发送十六进制字节（可重复），用于协议非法字段测试。
- `--hold MS` 建立连接后保持空闲，服务端因空闲超时关闭时以退出码 7 报告。

客户端退出码：`0` 成功；`1` 参数错误；`2` 连接失败；`3` 收齐响应前连接被
关闭；`4` 收到超过本端上限的帧；`5` 协议/顺序错误；`6` IO 错误或超时；
`7` 空闲连接被服务端关闭。

### 集成测试

```bash
# 独立脚本：真正拉起 serve 进程和多个 request / python 客户端
bash tests/integration/uxsock_integration.sh ./build/ipc_agent

# 或通过 ctest
ctest --test-dir build --output-on-failure
```

集成测试覆盖：多客户端并发且慢连接不阻塞他人、半包/粘包、超限拒绝、
非法字段拒绝、空闲超时、客户端中途断开、SIGTERM 优雅退出、宽限期超时
失败（退出码 2）、Socket 权限 0600、占用路径保护与失效节点回收。
所有结论均可从进程退出码与输出中观察。运行需要 `bash` 与 `python3`。

## 依赖

- GCC 9+ 或 Clang 10+
- CMake 3.16+
- POSIX 兼容系统（`ipc_agent` 的 serve/request 模式仅支持 Linux）
