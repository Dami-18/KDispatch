// Arm A: gRPC-style baseline.
//
// A fixed pool of I/O threads, each owning a shard of the connections. Every
// thread does its own byte-stream reassembly AND runs the RPC work for the
// messages it reassembles. That coupling is the mechanism under test: a worker
// busy with a large message cannot serve a small message that is already
// complete on another connection in its shard.
//
// Usage:
//   server_userspace --port 9000 --workers 4 --out results/serverA.json

#include "proto.hpp"
#include "server_stats.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <memory>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace kd;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

struct Conn {
    int fd = -1;
    std::vector<char> in;
    std::size_t in_off = 0;
    std::vector<char> out;
    std::size_t out_off = 0;
    bool want_out = false;
};

void set_nonblock(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

class Worker {
public:
    explicit Worker(int id) : id_(id) {
        epfd_ = ::epoll_create1(0);
        evfd_ = ::eventfd(0, EFD_NONBLOCK);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = evfd_;
        ::epoll_ctl(epfd_, EPOLL_CTL_ADD, evfd_, &ev);
    }

    void hand_off(int fd) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.push_back(fd);
        }
        std::uint64_t one = 1;
        [[maybe_unused]] ssize_t n = ::write(evfd_, &one, sizeof(one));
    }

    void run() {
        std::vector<epoll_event> events(256);
        while (!g_stop.load(std::memory_order_relaxed)) {
            const std::uint64_t t0 = now_ns();
            int n = ::epoll_wait(epfd_, events.data(), (int)events.size(), 100);
            const std::uint64_t t1 = now_ns();
            stats_.idle_ns += t1 - t0;
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < n; ++i) {
                const int fd = events[i].data.fd;
                if (fd == evfd_) {
                    drain_pending();
                    continue;
                }
                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    close_conn(fd);
                    continue;
                }
                if (events[i].events & EPOLLOUT) flush(fd);
                if (events[i].events & EPOLLIN) on_readable(fd);
            }
            stats_.busy_ns += now_ns() - t1;
        }
    }

    const WorkerStats& stats() const { return stats_; }
    int id() const { return id_; }

private:
    void drain_pending() {
        std::uint64_t v = 0;
        while (::read(evfd_, &v, sizeof(v)) > 0) {}
        std::vector<int> fds;
        {
            std::lock_guard<std::mutex> lk(mu_);
            fds.swap(pending_);
        }
        for (int fd : fds) {
            if ((std::size_t)fd >= conns_.size()) conns_.resize(fd + 1);
            conns_[fd] = std::make_unique<Conn>();
            conns_[fd]->fd = fd;
            conns_[fd]->in.reserve(64 * 1024);
            set_nonblock(fd);
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = fd;
            ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
            ++stats_.conns;
        }
    }

    void close_conn(int fd) {
        ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        if ((std::size_t)fd < conns_.size()) conns_[fd].reset();
    }

    void rearm(Conn& c) {
        epoll_event ev{};
        ev.events = c.want_out ? (EPOLLIN | EPOLLOUT) : EPOLLIN;
        ev.data.fd = c.fd;
        ::epoll_ctl(epfd_, EPOLL_CTL_MOD, c.fd, &ev);
    }

    void flush(int fd) {
        if ((std::size_t)fd >= conns_.size() || !conns_[fd]) return;
        Conn& c = *conns_[fd];
        while (c.out_off < c.out.size()) {
            ssize_t n = ::write(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off);
            if (n > 0) {
                c.out_off += (std::size_t)n;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!c.want_out) { c.want_out = true; rearm(c); }
                return;
            }
            close_conn(fd);
            return;
        }
        c.out.clear();
        c.out_off = 0;
        if (c.want_out) { c.want_out = false; rearm(c); }
    }

    void on_readable(int fd) {
        if ((std::size_t)fd >= conns_.size() || !conns_[fd]) return;
        Conn& c = *conns_[fd];

        // --- userspace reassembly: raw bytes in, message boundaries recovered here
        char scratch[64 * 1024];
        for (;;) {
            ssize_t n = ::read(c.fd, scratch, sizeof(scratch));
            if (n > 0) {
                c.in.insert(c.in.end(), scratch, scratch + n);
                stats_.bytes_in += (std::uint64_t)n;
                continue;
            }
            if (n == 0) { close_conn(fd); return; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            close_conn(fd);
            return;
        }

        for (;;) {
            const std::size_t avail = c.in.size() - c.in_off;
            if (avail < sizeof(MsgHeader)) break;
            MsgHeader h{};
            std::memcpy(&h, c.in.data() + c.in_off, sizeof(h));
            const std::uint32_t len = msg_len(h);
            if (len < sizeof(MsgHeader) || len > MAX_MSG) { close_conn(fd); return; }
            if (avail < len) break;
            c.in_off += len;

            if (h.flags & FLAG_HELLO) continue;  // connection announcement, no reply

            // --- the RPC itself, on the same thread that did the reassembly
            busy_spin_us(h.work_us);
            ++stats_.msgs;

            MsgHeader r = h;
            r.flags |= FLAG_REPLY;
            set_msg_len(r, sizeof(MsgHeader));
            const char* p = reinterpret_cast<const char*>(&r);
            c.out.insert(c.out.end(), p, p + sizeof(r));
        }

        if (c.in_off == c.in.size()) {
            c.in.clear();
            c.in_off = 0;
        } else if (c.in_off > 1u << 20) {
            c.in.erase(c.in.begin(), c.in.begin() + (long)c.in_off);
            c.in_off = 0;
        }
        flush(fd);
    }

    int id_;
    int epfd_ = -1;
    int evfd_ = -1;
    std::mutex mu_;
    std::vector<int> pending_;
    std::vector<std::unique_ptr<Conn>> conns_;
    WorkerStats stats_{};
};

}  // namespace

int main(int argc, char** argv) {
    int port = 9000, nworkers = 4;
    std::string out;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--port") port = std::stoi(next());
        else if (a == "--workers") nworkers = std::stoi(next());
        else if (a == "--out") out = next();
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<std::thread> threads;
    for (int i = 0; i < nworkers; ++i) workers.push_back(std::make_unique<Worker>(i));
    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((std::uint16_t)port);
    if (::bind(lfd, (sockaddr*)&addr, sizeof(addr)) < 0) { std::perror("bind"); return 1; }
    if (::listen(lfd, 1024) < 0) { std::perror("listen"); return 1; }
    set_nonblock(lfd);
    std::fprintf(stderr, "[server_userspace] port=%d workers=%d\n", port, nworkers);

    // Spawned only once the listener is up: a failure below this point would
    // return from main with joinable threads, which aborts.
    for (int i = 0; i < nworkers; ++i) {
        threads.emplace_back([w = workers[i].get()] { w->run(); });
    }

    // Round-robin assignment: deterministic shards, unlike SO_REUSEPORT hashing.
    std::uint64_t next = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        int fd = ::accept(lfd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ::usleep(1000);
                continue;
            }
            if (errno == EINTR) continue;
            break;
        }
        workers[next++ % workers.size()]->hand_off(fd);
    }

    for (auto& t : threads) t.join();

    std::vector<WorkerStats> stats;
    stats.reserve(workers.size());
    for (auto& w : workers) stats.push_back(w->stats());
    write_server_json(out, "userspace", stats);

    return 0;
}
