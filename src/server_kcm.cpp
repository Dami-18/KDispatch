// Arm B: in-kernel message framing via KCM (Kernel Connection Multiplexor).
//
// Every accepted TCP socket is attached to a single KCM multiplexor. The kernel
// runs a BPF stream parser over each TCP receive path, so what reaches userspace
// is already a whole message -- one recv() per RPC, no reassembly buffers, and
// no connection ownership. Each worker holds its own cloned KCM socket, and the
// kernel hands the next complete message to whichever clone is ready.
//
// That is the difference under test. In arm A a worker is pinned to a shard of
// connections and cannot help with another shard's backlog; here any worker can
// take any message from any connection, so a large message occupies one worker
// instead of stalling everything queued behind it.
//
// Replies still go directly on the originating TCP socket, exactly as in arm A,
// so the only variable between the arms is the receive/dispatch path. KCM does
// not tell a reader which transport socket a message came from, which is why the
// protocol carries conn_id and every connection opens with a hello.
//
// Needs CAP_BPF (or CAP_SYS_ADMIN) to load the parser -- run under sudo unless
// kernel.unprivileged_bpf_disabled is 0.
//
// Usage:
//   sudo ./build/server_kcm --port 9000 --workers 4 --out results/serverB.json

#include "netutil.hpp"
#include "proto.hpp"
#include "server_stats.hpp"

#include <arpa/inet.h>
#include <linux/bpf.h>
#include <linux/kcm.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace kd;

namespace {

std::atomic<bool> g_stop{false};
// Published so workers can name the real cause when a connection dies.
std::atomic<int> g_rcvbuf{0};
void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

// conn_id -> transport fd, published by the accept thread, read by every worker.
std::array<std::atomic<int>, MAX_CONNS> g_conn_fd;
// Two workers can hold two messages from the same connection at once, so their
// replies must not interleave in the transport stream.
std::array<std::mutex, MAX_CONNS> g_conn_lock;

// The stream parser, as raw bytecode:
//
//   r6 = r1;                                // BPF_MOV64_REG(BPF_REG_6, BPF_REG_1)
//   r0 = ntohl(*(u32 *)(skb->data + 0));    // BPF_LD_ABS(BPF_W, 0)
//   exit;                                   // return value = message length
//
// The r6 move is not optional. LD_ABS is inherited from classic BPF and takes
// its skb pointer implicitly from r6, never from the context register r1, so
// without the prologue the verifier rejects the program with "R6 !read_ok".
//
// strparser reads the return value as the total length of the message, 0 as
// "need more bytes", negative as an error. Our header puts a big-endian total
// length at offset 0 precisely so this is a two-instruction program, which is
// small enough to emit directly and skip the clang/libbpf build dependency.
// See bpf/kcm_parser.bpf.c for the same program written in C.
int load_parser_prog() {
    bpf_insn insns[3];
    std::memset(insns, 0, sizeof(insns));
    insns[0].code = BPF_ALU64 | BPF_MOV | BPF_X;  // r6 = r1 (skb), for LD_ABS
    insns[0].dst_reg = BPF_REG_6;
    insns[0].src_reg = BPF_REG_1;
    insns[1].code = BPF_LD | BPF_W | BPF_ABS;     // r0 = ntohl(u32 at offset imm)
    insns[1].imm = 0;
    insns[2].code = BPF_JMP | BPF_EXIT;

    char log[8192];
    log[0] = '\0';
    union bpf_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.prog_type = BPF_PROG_TYPE_SOCKET_FILTER;
    attr.insn_cnt = 3;
    attr.insns = (std::uint64_t)(unsigned long)insns;
    attr.license = (std::uint64_t)(unsigned long)"GPL";
    attr.log_level = 1;
    attr.log_size = sizeof(log);
    attr.log_buf = (std::uint64_t)(unsigned long)log;

    const int fd = (int)::syscall(__NR_bpf, BPF_PROG_LOAD, &attr, sizeof(attr));
    if (fd < 0) {
        std::fprintf(stderr, "BPF_PROG_LOAD failed: %s\n", std::strerror(errno));
        if (log[0]) std::fprintf(stderr, "verifier log:\n%s\n", log);
        if (errno == EPERM)
            std::fprintf(stderr,
                "hint: loading a socket filter needs CAP_BPF; run 'sudo scripts/setcap.sh' "
                "or run under sudo\n");
        else if (errno == EACCES)
            std::fprintf(stderr,
                "hint: EACCES is the verifier rejecting the program, not a permissions "
                "problem -- sudo will not help. See the log above.\n");
    }
    return fd;
}

bool write_all(int fd, const char* p, std::size_t n) {
    while (n) {
        const ssize_t k = ::write(fd, p, n);
        if (k > 0) { p += k; n -= (std::size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        return false;  // peer gone; the run is ending
    }
    return true;
}

bool read_exactly(int fd, char* p, std::size_t n) {
    while (n) {
        const ssize_t k = ::read(fd, p, n);
        if (k > 0) { p += k; n -= (std::size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

void worker_loop(int kcm_fd, WorkerStats* stats) {
    // Bounded by the largest message the generator can send.
    const std::size_t cap = MAX_MSG;
    auto buf = std::make_unique<char[]>(cap);

    while (!g_stop.load(std::memory_order_relaxed)) {
        const std::uint64_t t0 = now_ns();
        // One recv, one whole message. No reassembly, no per-connection state.
        const ssize_t n = ::recv(kcm_fd, buf.get(), cap, 0);
        const std::uint64_t t1 = now_ns();
        stats->idle_ns += t1 - t0;

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            // EMSGSIZE here means strparser hit a message longer than
            // sk_rcvbuf and tore the connection down.
            if (errno == EMSGSIZE) {
                std::fprintf(stderr,
                    "recv: EMSGSIZE -- a message exceeded sk_rcvbuf (%d bytes); "
                    "strparser aborts the connection rather than buffering it\n",
                    g_rcvbuf.load(std::memory_order_relaxed));
            } else {
                std::fprintf(stderr, "recv: %s\n", std::strerror(errno));
            }
            break;
        }
        if (n == 0 || (std::size_t)n < sizeof(MsgHeader)) continue;

        MsgHeader h{};
        std::memcpy(&h, buf.get(), sizeof(h));
        stats->bytes_in += (std::uint64_t)n;

        if (!(h.flags & FLAG_HELLO)) {
            busy_spin_us(h.work_us);
            ++stats->msgs;

            if (h.conn_id < MAX_CONNS) {
                const int fd = g_conn_fd[h.conn_id].load(std::memory_order_acquire);
                if (fd >= 0) {
                    MsgHeader r = h;
                    r.flags |= FLAG_REPLY;
                    set_msg_len(r, sizeof(MsgHeader));
                    std::lock_guard<std::mutex> lk(g_conn_lock[h.conn_id]);
                    write_all(fd, reinterpret_cast<const char*>(&r), sizeof(r));
                }
            }
        }
        stats->busy_ns += now_ns() - t1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    int port = 9000, nworkers = 4;
    int rcvbuf_want = DEFAULT_RCVBUF;
    bool check_only = false;   // load the parser, report, exit
    std::string out;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--port") port = std::stoi(next());
        else if (a == "--workers") nworkers = std::stoi(next());
        else if (a == "--out") out = next();
        else if (a == "--check-bpf") check_only = true;
        else if (a == "--rcvbuf") rcvbuf_want = std::stoi(next());
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
    for (auto& v : g_conn_fd) v.store(-1, std::memory_order_relaxed);

    const int prog_fd = load_parser_prog();
    if (prog_fd < 0) return 1;
    if (check_only) {
        std::fprintf(stderr, "BPF stream parser loaded OK (prog_fd=%d)\n", prog_fd);
        return 0;
    }

    // One multiplexor; one cloned KCM socket per worker. The kernel picks a
    // ready clone for each completed message -- that is the work conservation.
    const int kcm0 = ::socket(AF_KCM, SOCK_DGRAM, KCMPROTO_CONNECTED);
    if (kcm0 < 0) {
        std::fprintf(stderr, "socket(AF_KCM): %s\nhint: modprobe kcm\n", std::strerror(errno));
        return 1;
    }

    std::vector<int> worker_fds;
    for (int i = 0; i < nworkers; ++i) {
        if (i == 0) { worker_fds.push_back(kcm0); continue; }
        kcm_clone c{};
        if (::ioctl(kcm0, SIOCKCMCLONE, &c) < 0) {
            std::fprintf(stderr, "SIOCKCMCLONE: %s\n", std::strerror(errno));
            return 1;
        }
        worker_fds.push_back(c.fd);
    }

    // Bounded recv so workers can observe g_stop instead of blocking forever.
    //
    // The KCM socket carries its own receive buffer, separate from the
    // transport socket's. Completed messages are queued against it, so it too
    // has to be able to hold a whole message -- otherwise delivery wedges with
    // no error counter incremented anywhere.
    for (int fd : worker_fds) {
        timeval tv{};
        tv.tv_usec = 100 * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        const int eff = set_rcvbuf(fd, rcvbuf_want);
        if (fd == worker_fds.front())
            std::fprintf(stderr, "[server_kcm] kcm socket sk_rcvbuf=%d bytes\n", eff);
    }

    std::vector<WorkerStats> stats((std::size_t)nworkers);
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((std::uint16_t)port);
    if (::bind(lfd, (sockaddr*)&addr, sizeof(addr)) < 0) { std::perror("bind"); return 1; }
    if (::listen(lfd, 1024) < 0) { std::perror("listen"); return 1; }
    {
        const int fl = ::fcntl(lfd, F_GETFL, 0);
        ::fcntl(lfd, F_SETFL, fl | O_NONBLOCK);
    }
    std::fprintf(stderr, "[server_kcm] port=%d workers=%d\n", port, nworkers);

    // Spawned only once the listener is up: a failure below this point would
    // return from main with joinable threads, which aborts.
    std::vector<std::thread> threads;
    for (int i = 0; i < nworkers; ++i) {
        threads.emplace_back(worker_loop, worker_fds[(std::size_t)i], &stats[(std::size_t)i]);
    }

    std::vector<int> attached;
    std::uint64_t nattached = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const int fd = ::accept(lfd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { ::usleep(1000); continue; }
            if (errno == EINTR) continue;
            break;
        }
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        const int rcvbuf = set_rcvbuf(fd, rcvbuf_want);
        if (nattached == 0) report_rcvbuf("server_kcm", rcvbuf);
        g_rcvbuf.store(rcvbuf, std::memory_order_relaxed);

        // Read the hello directly, before KCM owns this socket's receive path.
        timeval tv{};
        tv.tv_sec = 5;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        MsgHeader hello{};
        if (!read_exactly(fd, reinterpret_cast<char*>(&hello), sizeof(hello)) ||
            !(hello.flags & FLAG_HELLO) || hello.conn_id >= MAX_CONNS) {
            std::fprintf(stderr, "bad hello, dropping connection\n");
            ::close(fd);
            continue;
        }
        tv.tv_sec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        g_conn_fd[hello.conn_id].store(fd, std::memory_order_release);

        kcm_attach info{};
        info.fd = fd;
        info.bpf_fd = prog_fd;
        if (::ioctl(kcm0, SIOCKCMATTACH, &info) < 0) {
            std::fprintf(stderr, "SIOCKCMATTACH: %s\n", std::strerror(errno));
            ::close(fd);
            continue;
        }
        attached.push_back(fd);
        ++nattached;
    }

    for (auto& t : threads) t.join();

    // Connections are spread evenly by construction; record the count so the
    // JSON matches arm A's shape.
    for (auto& s : stats) s.conns = nattached / (std::uint64_t)std::max(1, nworkers);
    write_server_json(out, "kcm", stats);

    for (int fd : attached) ::close(fd);
    for (int fd : worker_fds) ::close(fd);
    ::close(lfd);
    return 0;
}
