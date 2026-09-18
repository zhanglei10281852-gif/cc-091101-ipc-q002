#ifndef SOCKET_PROTO_H
#define SOCKET_PROTO_H

/**
 * 本地代理帧协议 (Unix Domain Socket, SOCK_STREAM)
 *
 * 固定 12 字节头部，多字节字段一律使用大端序 (固定字节序)：
 *
 *   偏移  长度  含义
 *   0     2     magic: 'I' 'P' (0x49 0x50)
 *   2     1     协议版本: 1
 *   3     1     帧类型: 1=request, 2=response
 *   4     4     request_id (uint32, 大端)
 *   8     4     payload_len (uint32, 大端)
 *   12    N     payload (payload_len 字节)
 *
 * 服务端按 request_id 原样回传响应；同一连接内响应顺序与请求顺序一致。
 *
 * 请求 payload 约定：
 *   "SLEEP:<ms>:<text>"  - 服务端延迟 ms 毫秒后回复 "OK:<text>" (模拟慢请求)
 *   其他任意内容          - 服务端立即回复 "ECHO:<payload>"
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ipc {
namespace proto {

constexpr uint8_t MAGIC0 = 0x49;  // 'I'
constexpr uint8_t MAGIC1 = 0x50;  // 'P'
constexpr uint8_t VERSION = 1;
constexpr uint8_t TYPE_REQUEST = 1;
constexpr uint8_t TYPE_RESPONSE = 2;
constexpr std::size_t HEADER_SIZE = 12;

// 大端写入 uint32
inline void put_u32be(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

// 大端读取 uint32
inline uint32_t get_u32be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           (static_cast<uint32_t>(p[3]));
}

// 序列化帧头
inline void encode_header(uint8_t* out, uint8_t type, uint32_t request_id, uint32_t payload_len) {
    out[0] = MAGIC0;
    out[1] = MAGIC1;
    out[2] = VERSION;
    out[3] = type;
    put_u32be(out + 4, request_id);
    put_u32be(out + 8, payload_len);
}

// 构造完整帧 (头 + payload)
inline std::vector<uint8_t> build_frame(uint8_t type, uint32_t request_id, const std::string& payload) {
    std::vector<uint8_t> frame(HEADER_SIZE + payload.size());
    encode_header(frame.data(), type, request_id, static_cast<uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(frame.data() + HEADER_SIZE, payload.data(), payload.size());
    }
    return frame;
}

} // namespace proto
} // namespace ipc

#endif // SOCKET_PROTO_H
