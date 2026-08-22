// KDispatch wire protocol.
//
// Length-prefixed messages over TCP. The length field is the FIRST four bytes
// in network byte order so that a BPF stream parser (arm B / KCM) can compute
// the message length by reading a fixed offset -- see bpf/kcm_parser.bpf.c.
#pragma once

#include <cstdint>
#include <cstring>
#include <ctime>
#include <endian.h>

namespace kd {

inline constexpr std::uint16_t CLS_SMALL = 0;
inline constexpr std::uint16_t CLS_LARGE = 1;

inline constexpr std::uint16_t FLAG_REPLY = 1u << 0;
// First message on every connection, carrying its conn_id. Arm B needs it:
// KCM hands a worker a complete message with no indication of which transport
// socket it arrived on, so the server must learn conn_id -> fd up front in
// order to reply on the originating connection.
inline constexpr std::uint16_t FLAG_HELLO = 1u << 1;

// Upper bound on conn_id, sizing the server's conn_id -> fd table.
inline constexpr std::uint32_t MAX_CONNS = 4096;

// Guard rail for the reassembly path: refuse anything larger and drop the
// connection, so a corrupt length prefix cannot make us allocate wildly.
inline constexpr std::uint32_t MAX_MSG = 8u * 1024 * 1024;

// Upper bound on simulated RPC work. A corrupted header must not be able to
// park a worker for hours in the busy-spin.
inline constexpr std::uint32_t MAX_WORK_US = 1'000'000;

struct __attribute__((packed)) MsgHeader {
    std::uint32_t len;      // big-endian, TOTAL bytes including this header
    std::uint32_t req_id;   // unique per generator thread
    std::uint64_t send_ns;  // actual send timestamp (CLOCK_MONOTONIC_RAW)
    std::uint32_t work_us;  // server busy-spins this long, simulating RPC work
    std::uint16_t cls;      // CLS_SMALL / CLS_LARGE
    std::uint16_t flags;    // FLAG_REPLY / FLAG_HELLO
    std::uint32_t conn_id;  // which connection this arrived on
    std::uint32_t reserved;
};

static_assert(sizeof(MsgHeader) == 32, "wire header must stay 32 bytes");

inline std::uint32_t msg_len(const MsgHeader& h) { return be32toh(h.len); }
inline void set_msg_len(MsgHeader& h, std::uint32_t n) { h.len = htobe32(n); }

inline std::uint64_t now_ns() {
    ::timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

// Simulated RPC work. A spin rather than a sleep: sleeping would hand the core
// back to the scheduler and hide exactly the head-of-line blocking we are
// trying to measure.
inline void busy_spin_us(std::uint32_t us) {
    if (us == 0) return;
    const std::uint64_t deadline = now_ns() + std::uint64_t{us} * 1000ull;
    while (now_ns() < deadline) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#endif
    }
}

}  // namespace kd
