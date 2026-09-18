#!/usr/bin/env bash
#
# Unix Domain Socket 代理的 Linux 集成测试
#
# 真正启动独立的 ipc_agent serve 进程，再运行多个 ipc_agent request
# 客户端进程（外加一个原生 Python3 探针客户端）。所有结论都通过
# 进程退出码与输出内容观察：
#
#   1. 基本请求/响应与 request_id 顺序
#   2. 多客户端并发，且慢连接（SLEEP）不阻塞其它连接
#   3. 逐字节发送（半包输入）仍被正确重组
#   4. 单连接内多帧粘包一次发送
#   5. 帧长超限被拒绝（连接关闭，原因 frame-too-large）
#   6. 协议字段非法（reserved != 0 / type 非法）被关闭并记录原因
#   7. 空闲超时关闭连接并记录 idle-timeout
#   8. 客户端中途断开（半包后退出），服务端不崩溃
#   9. SIGTERM 优雅退出：在途请求在宽限期内完成，清理 Socket 文件
#  10. 宽限期超时：退出码明确非 0
#  11. Socket 节点权限为 0600；占用路径上存在活监听时拒绝启动
#
# 用法: uxsock_integration.sh <path-to-ipc_agent>
set -u

AGENT="${1:?usage: $0 <path-to-ipc_agent>}"
[ -x "$AGENT" ] || { echo "agent binary not executable: $AGENT" >&2; exit 1; }

WORKDIR="$(mktemp -d)"
SOCK="$WORKDIR/agent.sock"
SERVER_LOG="$WORKDIR/server.log"
SERVER_PID=""
PASS=0
FAIL=0

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

ok()   { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

start_server() { # args...
    "$AGENT" serve --path "$SOCK" "$@" >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    # 等待 socket 文件出现（最多 5s）
    for _ in $(seq 1 250); do
        [ -S "$SOCK" ] && return 0
        kill -0 "$SERVER_PID" 2>/dev/null || {
            echo "server exited early; log:" >&2
            cat "$SERVER_LOG" >&2
            return 1
        }
        sleep 0.02
    done
    echo "server did not create $SOCK" >&2
    return 1
}

stop_server() { # 预期: stop_server <expected-exit-code>
    local expected="${1:-0}"
    kill -TERM "$SERVER_PID" 2>/dev/null
    local code=0
    wait "$SERVER_PID" || code=$?
    SERVER_PID=""
    if [ "$code" -eq "$expected" ]; then
        return 0
    fi
    echo "  server exit code=$code expected=$expected" >&2
    return 1
}

server_log_has() { grep -q "$1" "$SERVER_LOG"; }

# ---- 1. 基本请求/响应与顺序 --------------------------------------------------
start_server || exit 1

OUT="$WORKDIR/basic.out"
"$AGENT" request --path "$SOCK" alpha beta gamma >"$OUT" 2>/dev/null
if [ $? -eq 0 ] \
   && [ "$(sed -n '1p' "$OUT")" = "RESPONSE 1 ACK:alpha" ] \
   && [ "$(sed -n '2p' "$OUT")" = "RESPONSE 2 ACK:beta" ] \
   && [ "$(sed -n '3p' "$OUT")" = "RESPONSE 3 ACK:gamma" ]; then
    ok "basic request/response carries request_id in order"
else
    bad "basic request/response; output was:"; cat "$OUT" >&2
fi

# ---- 2. 多客户端并发：慢连接不阻塞其它连接 -----------------------------------
T0=$(date +%s%N)
"$AGENT" request --path "$SOCK" "SLEEP:800:slow-1" "SLEEP:800:slow-2" \
    >"$WORKDIR/slow.out" 2>/dev/null &
SLOW_PID=$!
# 给慢客户端一点时间先占住服务端
sleep 0.1
"$AGENT" request --path "$SOCK" fast-1 fast-2 fast-3 \
    >"$WORKDIR/fast.out" 2>/dev/null
FAST_RC=$?
T1=$(date +%s%N)
FAST_MS=$(( (T1 - T0) / 1000000 ))
wait "$SLOW_PID" || true

if [ "$FAST_RC" -eq 0 ] \
   && [ "$(grep -c '^RESPONSE' "$WORKDIR/fast.out")" = 3 ] \
   && [ "$FAST_MS" -lt 600 ]; then
    ok "fast client not blocked by slow client (${FAST_MS}ms)"
else
    bad "fast client blocked or failed (rc=$FAST_RC, ${FAST_MS}ms)"
fi
grep -q "RESPONSE 1 ACK:slow-1" "$WORKDIR/slow.out" \
    && grep -q "RESPONSE 2 ACK:slow-2" "$WORKDIR/slow.out" \
    && ok "slow client got both responses in order" \
    || bad "slow client responses missing/out of order"

# ---- 3. 半包输入（逐字节发送）-----------------------------------------------
OUT="$WORKDIR/bytewise.out"
"$AGENT" request --path "$SOCK" --byte-at-a-time --delay-us 500 \
    "one-small-message" >"$OUT" 2>/dev/null
[ $? -eq 0 ] && [ "$(cat "$OUT")" = "RESPONSE 1 ACK:one-small-message" ] \
    && ok "byte-at-a-time (fragmented) frame reassembled" \
    || { bad "byte-at-a-time failed"; cat "$OUT" >&2; }

# ---- 4. 粘包：一次写入多帧（Python 探针）------------------------------------
python3 - "$SOCK" <<'PY'
import socket, struct, sys
path = sys.argv[1]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(path)
def frame(rid, payload, ftype=1, flags=0, reserved=0):
    body = payload if isinstance(payload, bytes) else payload.encode()
    total = 16 + len(body)
    return struct.pack(">IQBBH", total, rid, ftype, flags, reserved) + body
# 三帧拼成一次 write
blob = frame(1, b"coa-1") + frame(2, b"coa-2") + frame(3, b"coa-3")
s.sendall(blob)
for expect in (1, 2, 3):
    hdr = b""
    while len(hdr) < 16:
        chunk = s.recv(16 - len(hdr))
        if not chunk:
            sys.exit("server closed during header")
        hdr += chunk
    total, rid, ftype, flags, reserved = struct.unpack(">IQBBH", hdr)
    payload = b""
    while len(payload) < total - 16:
        payload += s.recv(total - 16 - len(payload))
    if rid != expect or payload != f"ACK:coa-{expect}".encode():
        sys.exit(f"unexpected response id={rid} payload={payload!r}")
s.close()
PY
[ $? -eq 0 ] && ok "coalesced frames (multiple frames in one send)" \
    || bad "coalesced frames failed"

# ---- 5. 帧长超限 -------------------------------------------------------------
# 5a. 服务端上限 64 字节，客户端构造 1 KiB 载荷
kill -TERM "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null
start_server --max-frame 64 || exit 1

python3 - "$SOCK" <<'PY'
import socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
payload = b"x" * 1024
total = 16 + len(payload)
s.sendall(struct.pack(">IQBBH", total, 1, 1, 0, 0) + payload)
# 服务端必须关闭连接
data = s.recv(16)
sys.exit(0 if data == b"" else 1)
PY
[ $? -eq 0 ] && ok "oversized frame: server closes connection" \
    || bad "oversized frame: connection not closed"
sleep 0.1
server_log_has "reason=frame-too-large" \
    && ok "oversized frame logged as frame-too-large" \
    || bad "oversized frame reason missing in server log"

# 5b. 声明帧长小于固定头部
python3 - "$SOCK" <<'PY'
import socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(struct.pack(">I", 4))
sys.exit(0 if s.recv(16) == b"" else 1)
PY
[ $? -eq 0 ] && server_log_has "reason=frame-length-invalid" \
    && ok "undersized frame rejected with distinct reason" \
    || bad "undersized frame not rejected properly"

# ---- 6. 协议字段非法 ---------------------------------------------------------
# 6a. reserved != 0
python3 - "$SOCK" <<'PY'
import socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(struct.pack(">IQBBH", 16, 1, 1, 0, 0xBEEF))
sys.exit(0 if s.recv(16) == b"" else 1)
PY
[ $? -eq 0 ] && server_log_has "reason=bad-reserved" \
    && ok "invalid reserved field rejected (bad-reserved)" \
    || bad "reserved field validation failed"

# 6b. type 非法
python3 - "$SOCK" <<'PY'
import socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(struct.pack(">IQBBH", 16, 1, 99, 0, 0))
sys.exit(0 if s.recv(16) == b"" else 1)
PY
[ $? -eq 0 ] && server_log_has "reason=bad-type" \
    && ok "invalid type field rejected (bad-type)" \
    || bad "type field validation failed"

# 6c. flags 非法（非 0）
python3 - "$SOCK" <<'PY'
import socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(struct.pack(">IQBBH", 16, 1, 1, 7, 0))
sys.exit(0 if s.recv(16) == b"" else 1)
PY
[ $? -eq 0 ] && server_log_has "reason=bad-flags" \
    && ok "invalid flags field rejected (bad-flags)" \
    || bad "flags field validation failed"

# ---- 7. 空闲超时 -------------------------------------------------------------
kill -TERM "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null
start_server --max-frame 64 --idle-timeout 300 || exit 1

"$AGENT" request --path "$SOCK" --hold 2000 >/dev/null 2>"$WORKDIR/hold.err"
HOLD_RC=$?
[ "$HOLD_RC" -eq 7 ] && ok "idle client observed server-side close (rc=7)" \
    || bad "idle timeout not observed (rc=$HOLD_RC)"
sleep 0.1
server_log_has "reason=idle-timeout" \
    && ok "idle connection closed and logged idle-timeout" \
    || bad "idle-timeout reason missing in server log"

# ---- 8. 客户端中途断开（只发半个帧就退出）-----------------------------------
python3 - "$SOCK" <<'PY'
import socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
# 声明帧长 64，但只发 10 个字节后立刻退出
s.sendall(struct.pack(">IQBBH", 64, 5, 1, 0, 0) + b"0123456789")
s.close()
PY
sleep 0.3
if kill -0 "$SERVER_PID" 2>/dev/null && server_log_has "reason=client-aborted"; then
    ok "half-frame client disconnect tolerated (client-aborted)"
else
    bad "half-frame disconnect not handled; server alive=$(kill -0 "$SERVER_PID" 2>/dev/null && echo yes || echo no)"
fi

# 服务端仍可正常服务新连接
"$AGENT" request --path "$SOCK" still-alive >/dev/null 2>&1
# max-frame=64 时 "still-alive" 载荷 11 字节，总帧 27 < 64，应成功
[ $? -eq 0 ] && ok "server serves new connections after client abort" \
    || bad "server unreachable after client abort"

# ---- 9. SIGTERM 优雅退出 -----------------------------------------------------
# 慢请求 SLEEP 600ms，宽限期 5s，应在宽限期内完成、退出码 0、socket 被清理
kill -TERM "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null
start_server --grace 5000 || exit 1
"$AGENT" request --path "$SOCK" "SLEEP:600:graceful" \
    >"$WORKDIR/grace.out" 2>/dev/null &
GPID=$!
sleep 0.2
if stop_server 0; then
    ok "SIGTERM: exit 0 after in-flight request finished"
else
    bad "SIGTERM graceful shutdown exit code"
fi
wait "$GPID"
GRACE_RC=$?
[ "$GRACE_RC" -eq 0 ] && grep -q "ACK:graceful" "$WORKDIR/grace.out" \
    && ok "in-flight response delivered during grace period" \
    || bad "in-flight response lost (client rc=$GRACE_RC)"
[ ! -e "$SOCK" ] && ok "socket node removed after graceful shutdown" \
    || bad "socket node left behind after shutdown"

# 关停后不再接收新连接
"$AGENT" request --path "$SOCK" --connect-timeout 500 --hold 0 x \
    >/dev/null 2>&1
[ $? -ne 0 ] && ok "new connections rejected after shutdown" \
    || bad "server accepted a connection after shutdown"

# ---- 10. 宽限期超时，明确失败 ------------------------------------------------
start_server --grace 500 || exit 1
"$AGENT" request --path "$SOCK" "SLEEP:5000:too-slow" >/dev/null 2>&1 &
TPID=$!
sleep 0.2
if stop_server 2; then
    ok "SIGTERM with expired grace exits non-zero (rc=2)"
else
    bad "grace-expiry exit code"
fi
wait "$TPID" 2>/dev/null
[ ! -e "$SOCK" ] && ok "socket node removed even on grace timeout" \
    || bad "socket node left behind on grace timeout"

# ---- 11. Socket 权限与路径占用保护 -------------------------------------------
start_server || exit 1
PERMS=$(stat -c '%a %U' "$SOCK")
[ "$(stat -c '%a' "$SOCK")" = "600" ] \
    && ok "socket node permissions are 0600 ($PERMS)" \
    || bad "socket node permissions not 0600 ($PERMS)"

# 路径上有活的监听者：第二个服务端必须启动失败，且不能移除该节点
"$AGENT" serve --path "$SOCK" >"$WORKDIR/second.log" 2>&1 &
SECOND_PID=$!
sleep 0.3
SECOND_RC=0
wait "$SECOND_PID" || SECOND_RC=$?
if [ "$SECOND_RC" -ne 0 ] && [ -S "$SOCK" ]; then
    ok "live socket path is not stolen by a second server"
else
    bad "second server should fail when path is live (rc=$SECOND_RC)"
fi

# 残留的失效 socket 节点应被自动回收
kill -TERM "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null
# 制造一个失效节点：绑定后不 listen/不 accept，connect 会 ECONNREFUSED
python3 - "$SOCK" <<'PY'
import os, socket, sys
try:
    os.unlink(sys.argv[1])
except FileNotFoundError:
    pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(sys.argv[1])
# 不 listen：节点存在但无监听者
PY
start_server || true
if [ -S "$SOCK" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    ok "stale (unlistened) socket node reclaimed on startup"
else
    bad "stale socket node was not reclaimed"
fi

# ---- 汇总 --------------------------------------------------------------------
echo
echo "================================================"
echo " integration: $PASS passed, $FAIL failed"
echo "================================================"
[ "$FAIL" -eq 0 ]
