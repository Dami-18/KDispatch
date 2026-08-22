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
| **B — KCM** | Linux `AF_KCM` + an eBPF length-prefix parser; the kernel delivers whole messages, any worker can take any message | done |
| **C — module** | custom kernel module on `strparser` with a single shared, work-conserving message queue | stretch |

Scoped-down exploration of ideas from
[*Rakaia: Scalable In-Kernel Scheduling for TCP-Based RPCs*](https://www.usenix.org/conference/osdi26/technical-sessions)
(Yang, Prasopoulos, Bugnion — EPFL, OSDI '26). This is not a reimplementation:
Rakaia does work-conserving scheduling in the TCP receive path with kTLS support
and a patched gRPC. KDispatch reproduces the *problem* and measures how far the
existing in-kernel message API gets you.

## Results

Workload: 128 B / 10 µs "small" RPCs, with 1% "large" ones carrying an identical
128 B payload but 2 ms of service time. Equal payloads on purpose — that isolates
**dispatch** from byte movement, so what is measured is scheduling, not copying.
32 connections, 30k msg/s offered, 5 s runs, median of 2 repeats.

![small-message p99 vs worker threads](docs/p99_small_by_workers.png)

| workers | A p99 | B p99 | B/A | A p99.9 | B p99.9 | B/A |
| --- | --- | --- | --- | --- | --- | --- |
| 2 | 5.4 ms | 111.6 ms | 20.5× | 8.5 ms | 150.6 ms | 17.7× |
| 4 | 2.7 ms | 181.6 ms | 66.8× | 4.6 ms | 257.8 ms | 55.8× |
| 8 | 1.8 ms | 1.7 ms | 0.95× | 3.3 ms | 2.8 ms | 0.82× |
| 16 | 1.3 ms | 1.3 ms | 1.01× | 2.0 ms | 1.7 ms | 0.85× |
| 32 | 0.57 ms | 0.61 ms | 1.07× | 1.5 ms | 1.2 ms | 0.83× |

### Moving framing into the kernel does not fix head-of-line blocking

This is the headline, and it is a negative result for KCM. Once there are enough
worker threads, arm B is **indistinguishable from userspace reassembly** on p99
(0.95×–1.07×) and only marginally better on p99.9 (~0.85×). Small messages still
wait roughly one large-message service time behind large ones.

Framing is not the bottleneck. KCM removes the reassembly buffers, the partial
reads, and the per-connection userspace state — and the tail barely moves. What
governs the tail is the *dispatch policy*, which KCM does not change.

That is precisely the gap Rakaia targets: the paper reports up to **5× higher
throughput-under-SLO than KCM**, and this measurement is an independent
reproduction of why that headroom exists. It is also the argument for arm C —
an in-kernel message API is necessary but not sufficient; the scheduling has to
be work-conserving.

### KCM collapses when workers are scarce

At 2 and 4 workers arm B is catastrophically worse — 20× and 67× the baseline
tail. A KCM socket is *reserved* to a connection while a message is outstanding,
so a worker busy running an RPC keeps its clone unavailable; with few clones,
`reserve_rx_kcm` finds no waiting socket, strparser pauses the transport, and the
backlog runs away. Arm A degrades gracefully over the same range.

Two honest caveats on this row:

- **It is bimodal.** Ad-hoc 4-worker runs during development produced ~2.5 ms,
  not 181 ms. Some runs enter the pathological state and some do not; the
  trigger is not yet identified.
- **Messages go missing.** At 2 and 4 workers, `recvd` falls short of `sent` by
  0.1–0.4% (414 and 1242 of ~300k). Every other configuration reconciles
  exactly. Unexplained, and it means those two rows should be read as "KCM
  misbehaves here", not as a trustworthy latency number.

The reservation mechanism above is a **hypothesis** consistent with the data, not
something this repo has proven. Confirming it needs tracing inside `kcm_rcv_strparser`.

### Two undocumented buffer ceilings

Getting arm B to run at all meant discovering that a message must fit in *two*
independent receive buffers:

1. **The transport socket's `sk_rcvbuf`.** `strparser` rejects anything longer
   with `EMSGSIZE` and **aborts the connection** — it does not degrade, it dies.
   With the stock `net.ipv4.tcp_rmem` default of 131072, every message above
   ~128 KB kills its connection.
2. **The KCM socket's own `sk_rcvbuf`.** Completed messages are queued against
   it. If a message does not fit, delivery **wedges silently** — no error
   counter anywhere in `/proc/net/kcm_stats` increments, the sender simply
   blocks forever in `write()`.

The second cost most of a debugging session. Both servers now size both buffers
and refuse to start a run that cannot complete.

### Arm A, for reference

Sweeping the large-message fraction at 32 connections / 4 workers, with 256 KB
large payloads (the earlier workload, before payloads were equalized):

| large% | p50 | p99 | p99.9 |
| --- | --- | --- | --- |
| 0.1% | 27.1 µs | 688 µs | 2.00 ms |
| 1% | 29.2 µs | 2.95 ms | 5.24 ms |
| 4% | 1.08 ms | 14.7 ms | 23.6 ms |

Connection count on its own is a flat axis for arm A: at fixed aggregate rate and
worker count, adding connections does not change how often a small message lands
behind a large one. Kept as a control.

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
```

Arm B loads a BPF stream parser, which needs `CAP_BPF`. Grant it once per build
(file capabilities live on the inode, so any relink clears them):

```bash
sudo scripts/setcap.sh
./build/server_kcm --check-bpf        # should print "loaded OK"
```

Side-by-side comparison at one operating point:

```bash
scripts/smoke_kcm.sh
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
