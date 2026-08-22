# KDispatch

**In-kernel, TCP-aware message dispatch for RPCs — and a measurement rig that shows why it matters.**

TCP gives you a byte stream, not messages. RPC frameworks therefore reassemble
message boundaries in userspace, on I/O threads that own a shard of the
connections. That couples *reassembly* to *execution*: a worker busy with one
large message cannot serve a small message that is already complete on another
connection in its shard. The result is head-of-line (HOL) blocking, and it gets
worse as connections-per-worker grows.

KDispatch measures that effect and compares three ways of getting messages from
the wire to a worker thread.

| Arm | What it does | Status |
| --- | --- | --- |
| **A — userspace** | epoll server, per-connection reassembly, workers sharded by connection (what gRPC-style stacks do) | done |
| **B — KCM** | Linux `AF_KCM` + an eBPF length-prefix parser; the kernel delivers whole messages, any worker can take any message | planned |
| **C — module** | custom kernel module on `strparser` with a single shared, work-conserving message queue | stretch |

Scoped-down exploration of ideas from
[*Rakaia: Scalable In-Kernel Scheduling for TCP-Based RPCs*](https://www.usenix.org/conference/osdi26/technical-sessions)
(Yang, Prasopoulos, Bugnion — EPFL, OSDI '26). This is not a reimplementation:
Rakaia does work-conserving scheduling in the TCP receive path with kTLS support
and a patched gRPC. KDispatch reproduces the *problem* and measures how far the
existing in-kernel message API gets you.

## Early results (arm A)

128 B / 10 µs "small" messages, with a fraction of 256 KB / 2 ms "large" ones
mixed in. 32 connections, 30k msg/s offered, 4-second runs.

Sweeping the large-message fraction — the tail tracks one large-message
service time and then collapses once workers saturate:

| large% | p50 | p99 | p99.9 |
| --- | --- | --- | --- |
| 0.1% | 27.1 µs | 688 µs | 2.00 ms |
| 1% | 29.2 µs | 2.95 ms | 5.24 ms |
| 4% | 1.08 ms | 14.7 ms | 23.6 ms |

Sweeping worker threads at 1% large:

| workers | p50 | p99 | p99.9 |
| --- | --- | --- | --- |
| 1 | 5.11 ms | 36.7 ms | 51.4 ms |
| 2 | 36.9 µs | 5.77 ms | 9.96 ms |
| 4 | 29.2 µs | 2.95 ms | 5.37 ms |
| 8 | 28.2 µs | 1.90 ms | 3.60 ms |
| 16 | 28.2 µs | 1.38 ms | 2.10 ms |

The second table is the point of the project. Even at 16 workers, p99 is
**1.38 ms against a p50 of 28 µs** — adding threads only *dilutes* head-of-line
blocking, it never removes it, because reassembly and execution are structurally
coupled to the same thread. Closing that gap is what arm B is for.

Note that connection count on its own is a flat axis for arm A: at a fixed
aggregate rate and worker count, adding connections does not change how often a
small message lands behind a large one. It is kept as a control.

## Layout

```
include/proto.hpp   wire format (length-prefixed, BPF-parseable header)
include/hist.hpp    log-bucketed latency histogram, ~3% relative precision
src/server_userspace.cpp   arm A
src/loadgen.cpp            open-loop generator + reply reader
bpf/                       KCM stream parser (arm B)
module/                    strparser dispatcher (arm C)
scripts/run_sweep.sh       connection-count sweep
scripts/plot.py            graphs from results/*.json
```

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

./build/server_userspace --port 9000 --workers 4 --out results/srv.json &
./build/loadgen --port 9000 --conns 64 --threads 4 \
                --rate 50000 --duration 30 --warmup 5 --out results/run.json
kill %1
```

Full sweep and graphs:

```bash
scripts/run_sweep.sh userspace
python3 scripts/plot.py results/ -o figs/
```

## Measurement notes

The numbers are only worth anything if the rig is honest, so:

- **Open loop.** The generator sends on a fixed-rate schedule with exponential
  gaps, never "next request after previous reply". Closed-loop generators
  throttle themselves exactly when the server is struggling and erase the tail
  — coordinated omission.
- **Two latencies per message.** `small` measures from the actual `write()`;
  `small_ol` measures from the *scheduled* send time. When the generator itself
  falls behind, these diverge and `sched_delay` says by how much. Graphs use the
  `_ol` numbers.
- **Warmup discarded** (default 5 s), reported in every result file.
- **Work is spun, not slept.** Sleeping would hand the core back to the
  scheduler and hide the blocking under test.
- **Worker busy% spread** (max − min across workers) is the direct evidence for
  work conservation: a sharded design leaves workers idle while messages queue.

For real runs, pin the cores and fix the clock:

```bash
sudo cpupower frequency-set -g performance
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
```

## Requirements

- Linux with `CONFIG_AF_KCM=m` and `CONFIG_STREAM_PARSER=y` (needed for arm B)
- C++23 compiler, CMake ≥ 3.20
- clang + libbpf for the arm B parser
- kernel headers for arm C
