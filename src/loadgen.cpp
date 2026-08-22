// Open-loop load generator.
//
// Messages are emitted on a fixed-rate schedule (exponential inter-arrival
// gaps) regardless of whether earlier replies have come back. A closed-loop
// generator -- send the next request only after the previous reply -- would
// throttle itself exactly when the server is struggling and hide the tail we
// are trying to measure. That failure mode is coordinated omission.
//
// Two latencies are reported per size class:
//   actual : reply_time - time the bytes actually hit write()
//   ol     : reply_time - time the message was SCHEDULED to be sent
// They diverge when the generator itself falls behind; "ol" is the honest
// number and "sched_delay" tells you how much of it is our own fault.
//
// Usage:
//   loadgen --host 127.0.0.1 --port 9000 --conns 64 --rate 50000
//           --duration 30 --warmup 5 --out results/run.json

#include "hist.hpp"
#include "proto.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace kd;

namespace {

constexpr std::size_t RING = 1u << 16;  // req_id -> intended send time

struct Cfg {
    std::string host = "127.0.0.1";
    int port = 9000;
    int conns = 64;
    int threads = 4;
    double rate = 50000;      // total msgs/sec across all threads
    double duration = 30;     // seconds
    double warmup = 5;        // seconds of samples discarded
    double large_pct = 1.0;   // percent of messages that are "large"
    int small_size = 128;     // total bytes on the wire, incl. header
    int large_size = 256 * 1024;
    int small_work_us = 10;
    int large_work_us = 2000;
    std::string out;
    std::string arm = "userspace";
};

struct ThreadResult {
    Histogram small_actual, large_actual, small_ol, large_ol, sched_delay;
    std::uint64_t sent = 0, recvd = 0, bytes_sent = 0;
    std::uint64_t max_sched_delay_ns = 0;
};

int dial(const Cfg& c) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((std::uint16_t)c.port);
    if (::inet_pton(AF_INET, c.host.c_str(), &a.sin_addr) != 1) return -1;
    if (::connect(fd, (sockaddr*)&a, sizeof(a)) < 0) { ::close(fd); return -1; }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

bool write_all(int fd, const char* p, std::size_t n) {
    while (n) {
        ssize_t k = ::write(fd, p, n);
        if (k > 0) { p += k; n -= (std::size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

void wait_until(std::uint64_t target_ns) {
    for (;;) {
        const std::uint64_t now = now_ns();
        if (now >= target_ns) return;
        const std::uint64_t left = target_ns - now;
        if (left > 100'000) {  // >100us: give the core back
            ::timespec ts{};
            ts.tv_sec = 0;
            ts.tv_nsec = (long)(left - 60'000);
            ::nanosleep(&ts, nullptr);
        }
    }
}

// Reader half: drains replies for one thread's shard of connections.
void reader_loop(std::vector<int> fds, const std::uint64_t* intended,
                 std::uint64_t measure_from_ns, ThreadResult* res,
                 std::atomic<bool>* stop) {
    int epfd = ::epoll_create1(0);
    for (int fd : fds) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        ::epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }
    std::vector<epoll_event> events(64);
    std::vector<std::vector<char>> buf(fds.size());
    std::vector<int> idx_of(1024, -1);
    for (std::size_t i = 0; i < fds.size(); ++i) {
        if ((std::size_t)fds[i] >= idx_of.size()) idx_of.resize(fds[i] + 1, -1);
        idx_of[fds[i]] = (int)i;
    }

    while (!stop->load(std::memory_order_relaxed)) {
        int n = ::epoll_wait(epfd, events.data(), (int)events.size(), 50);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            auto& b = buf[idx_of[fd]];
            char scratch[16 * 1024];
            ssize_t k = ::read(fd, scratch, sizeof(scratch));
            if (k <= 0) continue;
            b.insert(b.end(), scratch, scratch + k);
            const std::uint64_t rx = now_ns();
            std::size_t off = 0;
            while (b.size() - off >= sizeof(MsgHeader)) {
                MsgHeader h{};
                std::memcpy(&h, b.data() + off, sizeof(h));
                if (msg_len(h) != sizeof(MsgHeader)) { off = b.size(); break; }
                off += sizeof(MsgHeader);
                ++res->recvd;
                if (rx < measure_from_ns) continue;  // warmup
                const std::uint64_t sched = intended[h.req_id % RING];
                const std::uint64_t lat_actual = rx > h.send_ns ? rx - h.send_ns : 0;
                const std::uint64_t lat_ol = rx > sched ? rx - sched : 0;
                if (h.cls == CLS_LARGE) {
                    res->large_actual.record(lat_actual);
                    res->large_ol.record(lat_ol);
                } else {
                    res->small_actual.record(lat_actual);
                    res->small_ol.record(lat_ol);
                }
            }
            if (off) b.erase(b.begin(), b.begin() + (long)off);
        }
    }
    ::close(epfd);
}

void run_thread(const Cfg& cfg, int tid, int nconn, ThreadResult* res) {
    std::vector<int> fds;
    for (int i = 0; i < nconn; ++i) {
        int fd = dial(cfg);
        if (fd < 0) { std::fprintf(stderr, "connect failed\n"); std::exit(1); }
        fds.push_back(fd);
    }

    auto intended = std::make_unique<std::uint64_t[]>(RING);
    std::memset(intended.get(), 0, RING * sizeof(std::uint64_t));

    const std::uint64_t start = now_ns();
    const std::uint64_t measure_from = start + (std::uint64_t)(cfg.warmup * 1e9);
    const std::uint64_t end = start + (std::uint64_t)(cfg.duration * 1e9);

    std::atomic<bool> stop{false};
    std::thread reader(reader_loop, fds, intended.get(), measure_from, res, &stop);

    std::mt19937_64 rng(0x5eed ^ (std::uint64_t)tid);
    std::uniform_real_distribution<double> uni(1e-12, 1.0);
    const double rate_per_thread = cfg.rate / std::max(1, cfg.threads);

    std::vector<char> payload(std::max(cfg.small_size, cfg.large_size), 'x');
    std::uint32_t req_id = 0;
    double next = (double)start;
    int rr = 0;

    while (now_ns() < end) {
        next += -std::log(uni(rng)) / rate_per_thread * 1e9;
        const std::uint64_t target = (std::uint64_t)next;
        wait_until(target);

        const bool large = (uni(rng) * 100.0) < cfg.large_pct;
        const std::uint32_t len = large ? (std::uint32_t)cfg.large_size
                                        : (std::uint32_t)cfg.small_size;

        MsgHeader h{};
        set_msg_len(h, len);
        h.req_id = req_id;
        h.work_us = large ? (std::uint32_t)cfg.large_work_us
                          : (std::uint32_t)cfg.small_work_us;
        h.cls = large ? CLS_LARGE : CLS_SMALL;
        h.flags = 0;

        intended[req_id % RING] = target;
        const std::uint64_t actual = now_ns();
        h.send_ns = actual;
        const std::uint64_t delay = actual > target ? actual - target : 0;
        res->sched_delay.record(delay);
        res->max_sched_delay_ns = std::max(res->max_sched_delay_ns, delay);

        std::memcpy(payload.data(), &h, sizeof(h));
        const int fd = fds[rr++ % fds.size()];
        if (!write_all(fd, payload.data(), len)) break;
        ++res->sent;
        res->bytes_sent += len;
        ++req_id;
    }

    // Let in-flight replies land before tearing the reader down.
    ::usleep(300 * 1000);
    stop.store(true, std::memory_order_relaxed);
    reader.join();
    for (int fd : fds) ::close(fd);
}

void emit(FILE* f, const char* name, const Histogram& h) {
    std::fprintf(f,
        "    \"%s\": {\"count\": %llu, \"p50_ns\": %llu, \"p99_ns\": %llu, "
        "\"p999_ns\": %llu, \"max_ns\": %llu, \"mean_ns\": %.0f}",
        name, (unsigned long long)h.count(),
        (unsigned long long)h.percentile(0.50),
        (unsigned long long)h.percentile(0.99),
        (unsigned long long)h.percentile(0.999),
        (unsigned long long)h.max(), h.mean());
}

}  // namespace

int main(int argc, char** argv) {
    Cfg cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--host") cfg.host = next();
        else if (a == "--port") cfg.port = std::stoi(next());
        else if (a == "--conns") cfg.conns = std::stoi(next());
        else if (a == "--threads") cfg.threads = std::stoi(next());
        else if (a == "--rate") cfg.rate = std::stod(next());
        else if (a == "--duration") cfg.duration = std::stod(next());
        else if (a == "--warmup") cfg.warmup = std::stod(next());
        else if (a == "--large-pct") cfg.large_pct = std::stod(next());
        else if (a == "--small-size") cfg.small_size = std::stoi(next());
        else if (a == "--large-size") cfg.large_size = std::stoi(next());
        else if (a == "--small-work-us") cfg.small_work_us = std::stoi(next());
        else if (a == "--large-work-us") cfg.large_work_us = std::stoi(next());
        else if (a == "--arm") cfg.arm = next();
        else if (a == "--out") cfg.out = next();
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }
    if (cfg.threads > cfg.conns) cfg.threads = cfg.conns;
    if (cfg.small_size < (int)sizeof(MsgHeader) || cfg.large_size < (int)sizeof(MsgHeader)) {
        std::fprintf(stderr, "sizes must be >= %zu\n", sizeof(MsgHeader));
        return 2;
    }

    std::vector<ThreadResult> results((std::size_t)cfg.threads);
    std::vector<std::thread> ts;
    const int base = cfg.conns / cfg.threads, extra = cfg.conns % cfg.threads;
    for (int t = 0; t < cfg.threads; ++t) {
        const int n = base + (t < extra ? 1 : 0);
        ts.emplace_back(run_thread, std::cref(cfg), t, n, &results[(std::size_t)t]);
    }
    for (auto& t : ts) t.join();

    ThreadResult agg;
    for (auto& r : results) {
        agg.small_actual.merge(r.small_actual);
        agg.large_actual.merge(r.large_actual);
        agg.small_ol.merge(r.small_ol);
        agg.large_ol.merge(r.large_ol);
        agg.sched_delay.merge(r.sched_delay);
        agg.sent += r.sent;
        agg.recvd += r.recvd;
        agg.bytes_sent += r.bytes_sent;
        agg.max_sched_delay_ns = std::max(agg.max_sched_delay_ns, r.max_sched_delay_ns);
    }

    const double achieved = (double)agg.sent / cfg.duration;
    const double goodput_mbps = (double)agg.bytes_sent * 8.0 / cfg.duration / 1e6;

    FILE* f = cfg.out.empty() ? stdout : std::fopen(cfg.out.c_str(), "w");
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"arm\": \"%s\",\n  \"conns\": %d,\n  \"gen_threads\": %d,\n",
                 cfg.arm.c_str(), cfg.conns, cfg.threads);
    std::fprintf(f, "  \"offered_rate\": %.0f,\n  \"achieved_rate\": %.0f,\n",
                 cfg.rate, achieved);
    std::fprintf(f, "  \"duration_s\": %.1f,\n  \"warmup_s\": %.1f,\n",
                 cfg.duration, cfg.warmup);
    std::fprintf(f, "  \"sent\": %llu,\n  \"recvd\": %llu,\n",
                 (unsigned long long)agg.sent, (unsigned long long)agg.recvd);
    std::fprintf(f, "  \"goodput_mbps\": %.1f,\n", goodput_mbps);
    std::fprintf(f, "  \"large_pct\": %.2f,\n", cfg.large_pct);
    std::fprintf(f, "  \"small_size\": %d,\n  \"large_size\": %d,\n",
                 cfg.small_size, cfg.large_size);
    std::fprintf(f, "  \"small_work_us\": %d,\n  \"large_work_us\": %d,\n",
                 cfg.small_work_us, cfg.large_work_us);
    std::fprintf(f, "  \"max_sched_delay_ns\": %llu,\n",
                 (unsigned long long)agg.max_sched_delay_ns);
    std::fprintf(f, "  \"latency\": {\n");
    emit(f, "small", agg.small_actual);        std::fprintf(f, ",\n");
    emit(f, "large", agg.large_actual);        std::fprintf(f, ",\n");
    emit(f, "small_ol", agg.small_ol);         std::fprintf(f, ",\n");
    emit(f, "large_ol", agg.large_ol);         std::fprintf(f, ",\n");
    emit(f, "sched_delay", agg.sched_delay);   std::fprintf(f, "\n  }\n}\n");
    if (f != stdout) std::fclose(f);
    return 0;
}
