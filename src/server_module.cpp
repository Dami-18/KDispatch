// Arm C: work-conserving in-kernel dispatch via the kdispatch module.
//
// Every accepted TCP socket is handed to the module, which runs strparser over
// its receive path and puts each completed message on ONE shared queue. Workers
// read() the device and take whatever is at the head.
//
// That is the whole difference from arms A and B. Arm A binds a worker to a
// shard of connections; KCM binds a KCM socket to a connection while a message
// is outstanding. Here nothing is bound: a message waits only if every worker is
// genuinely busy, which is what "work-conserving" means.
//
// Replies go directly on the originating TCP socket, as in both other arms, so
// the receive/dispatch path stays the only variable.
//
// Needs the module loaded:  sudo scripts/load_module.sh
//
// Usage:
//   ./build/server_module --port 9000 --workers 4 --out results/serverC.json

#include "kdispatch_uapi.h"
#include "netutil.hpp"
#include "proto.hpp"
#include "server_stats.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace kd;

namespace {

std::atomic<bool> g_stop{false};
int g_devfd = -1;

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
    // Unblock workers parked in read(); they cannot rely on signal delivery.
    if (g_devfd >= 0) ::ioctl(g_devfd, KD_SHUTDOWN);
}

std::array<std::atomic<int>, MAX_CONNS> g_conn_fd;
// Two workers can hold two messages from one connection at once, so their
// replies must not interleave in the transport stream.
std::array<std::mutex, MAX_CONNS> g_conn_lock;

bool write_all(int fd, const char* p, std::size_t n) {
    while (n) {
        const ssize_t k = ::write(fd, p, n);
        if (k > 0) { p += k; n -= (std::size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        return false;
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

void worker_loop(int devfd, WorkerStats* stats) {
    const std::size_t cap = MAX_MSG;
    auto buf = std::make_unique<char[]>(cap);

    while (!g_stop.load(std::memory_order_relaxed)) {
        const std::uint64_t t0 = now_ns();
        // One read, one whole message, from any connection. No ownership.
        const ssize_t n = ::read(devfd, buf.get(), cap);
        const std::uint64_t t1 = now_ns();
        stats->idle_ns += t1 - t0;

        if (n == 0) break;                       // module draining
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            std::fprintf(stderr, "read: %s\n", std::strerror(errno));
            break;
        }
        if ((std::size_t)n < sizeof(MsgHeader)) continue;

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
    std::string out;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--port") port = std::stoi(next());
        else if (a == "--workers") nworkers = std::stoi(next());
        else if (a == "--out") out = next();
        else if (a == "--rcvbuf") rcvbuf_want = std::stoi(next());
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }

    for (auto& v : g_conn_fd) v.store(-1, std::memory_order_relaxed);

    g_devfd = ::open(KD_DEVICE_PATH, O_RDONLY);
    if (g_devfd < 0) {
        std::fprintf(stderr, "open %s: %s\nhint: sudo scripts/load_module.sh\n",
                     KD_DEVICE_PATH, std::strerror(errno));
        return 1;
    }

    // Signal handlers are installed without SA_RESTART so a blocking accept()
    // returns rather than resuming; workers are woken by KD_SHUTDOWN instead.
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

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
    std::fprintf(stderr, "[server_module] port=%d workers=%d\n", port, nworkers);

    // Spawned only once the listener is up: a failure below this point would
    // return from main with joinable threads, which aborts.
    std::vector<WorkerStats> stats((std::size_t)nworkers);
    std::vector<std::thread> threads;
    for (int i = 0; i < nworkers; ++i)
        threads.emplace_back(worker_loop, g_devfd, &stats[(std::size_t)i]);

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
        if (nattached == 0) report_rcvbuf("server_module", rcvbuf);

        // Read the hello before the module takes over this socket's receive path.
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

        kd_attach info{};
        info.fd = fd;
        if (::ioctl(g_devfd, KD_ATTACH, &info) < 0) {
            std::fprintf(stderr, "KD_ATTACH: %s\n", std::strerror(errno));
            ::close(fd);
            continue;
        }
        attached.push_back(fd);
        ++nattached;
    }

    ::ioctl(g_devfd, KD_SHUTDOWN);
    for (auto& t : threads) t.join();

    kd_stats ks{};
    if (::ioctl(g_devfd, KD_STATS, &ks) == 0) {
        std::fprintf(stderr,
            "[server_module] module: msgs=%llu qmax=%u pauses=%u aborts=%u conns=%u\n",
            (unsigned long long)ks.msgs, ks.qmax, ks.pauses, ks.aborts, ks.conns);
    }

    for (auto& s : stats) s.conns = nattached / (std::uint64_t)std::max(1, nworkers);
    write_server_json(out, "module", stats);

    for (int fd : attached) ::close(fd);
    ::close(g_devfd);
    ::close(lfd);
    return 0;
}
