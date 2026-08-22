// Per-worker accounting shared by every server arm, so the arms emit
// byte-identical JSON and plot.py needs no per-arm special cases.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace kd {

struct WorkerStats {
    std::uint64_t busy_ns = 0;   // time executing RPC work
    std::uint64_t idle_ns = 0;   // time blocked waiting for a message
    std::uint64_t msgs = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t conns = 0;
};

// busy% spread across workers is the direct evidence for work conservation:
// a connection-sharded design leaves some workers idle while messages queue
// behind others, so max-min stays wide. A shared queue keeps it narrow.
inline void write_server_json(const std::string& path, const char* arm,
                              const std::vector<WorkerStats>& per_worker) {
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;

    std::uint64_t msgs = 0;
    double busy_sum = 0, busy_min = 1e9, busy_max = -1e9;

    std::fprintf(f, "{\n  \"arm\": \"%s\",\n  \"workers\": %zu,\n", arm, per_worker.size());
    std::fprintf(f, "  \"per_worker\": [");
    for (std::size_t i = 0; i < per_worker.size(); ++i) {
        const WorkerStats& s = per_worker[i];
        const double tot = double(s.busy_ns + s.idle_ns);
        const double busy_pct = tot > 0 ? 100.0 * double(s.busy_ns) / tot : 0.0;
        msgs += s.msgs;
        busy_sum += busy_pct;
        busy_min = std::min(busy_min, busy_pct);
        busy_max = std::max(busy_max, busy_pct);
        std::fprintf(f,
            "%s\n    {\"id\": %zu, \"msgs\": %llu, \"conns\": %llu, \"busy_pct\": %.2f}",
            i ? "," : "", i, (unsigned long long)s.msgs,
            (unsigned long long)s.conns, busy_pct);
    }
    std::fprintf(f, "\n  ],\n");

    const double mean = per_worker.empty() ? 0.0 : busy_sum / double(per_worker.size());
    std::fprintf(f, "  \"msgs\": %llu,\n", (unsigned long long)msgs);
    std::fprintf(f, "  \"worker_busy_pct_mean\": %.2f,\n", mean);
    std::fprintf(f, "  \"worker_busy_pct_min\": %.2f,\n", busy_min);
    std::fprintf(f, "  \"worker_busy_pct_max\": %.2f,\n", busy_max);
    std::fprintf(f, "  \"worker_busy_pct_spread\": %.2f\n}\n", busy_max - busy_min);
    std::fclose(f);
}

}  // namespace kd
