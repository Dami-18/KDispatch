// Socket tuning shared by both server arms, so the only thing differing
// between them stays the receive/dispatch path.
#pragma once

#include <cstdint>
#include <cstdio>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

namespace kd {

// strparser refuses any message longer than the transport socket's receive
// buffer -- net/strparser/strparser.c does:
//
//     } else if (len > strp->sk->sk_rcvbuf) {
//             STRP_STATS_INCR(strp->stats.msg_too_big);
//             strp_parser_err(strp, -EMSGSIZE, desc);
//
// which aborts the connection. sk_rcvbuf defaults to net.ipv4.tcp_rmem[1]
// (131072 on a stock kernel), comfortably under a 256 KB message, so arm B
// silently wedges without this. Arm A does not need the larger buffer, but
// gets exactly the same one: an unequal socket configuration would confound
// the comparison.
//
// Deliberately plain SO_RCVBUF rather than SO_RCVBUFFORCE. The forcing variant
// would let arm B exceed net.core.rmem_max because it holds CAP_NET_ADMIN for
// the BPF loader, while unprivileged arm A could not follow -- the arms would
// end up with different buffers. Both are capped at 2 * rmem_max instead.
inline int set_rcvbuf(int fd, int want) {
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
    int effective = 0;
    ::socklen_t len = sizeof(effective);
    ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &effective, &len);
    return effective;
}

inline constexpr int DEFAULT_RCVBUF = 8 * 1024 * 1024;  // capped by rmem_max

// Bound how long a reply write may block.
//
// Arms B and C reply with a plain blocking write() from a worker thread. If the
// client vanishes mid-run with a full send buffer, that write never returns:
// the worker is stuck, SIGTERM cannot land, and the server has to be SIGKILLed
// while the sweep harness waits on it forever. A send timeout turns that into a
// failed write on one connection instead of a wedged server.
inline void set_sndtimeo(int fd, int seconds) {
    ::timeval tv{};
    tv.tv_sec = seconds;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

inline constexpr int DEFAULT_SNDTIMEO_S = 5;

// Report the effective buffer once, with the ceiling it implies on message
// size. A message above this is not a slow path -- it kills the connection.
inline void report_rcvbuf(const char* arm, int effective) {
    std::fprintf(stderr,
        "[%s] sk_rcvbuf=%d bytes; messages larger than this are rejected by "
        "strparser (raise net.core.rmem_max to lift it)\n", arm, effective);
}

}  // namespace kd
