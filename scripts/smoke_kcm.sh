#!/usr/bin/env bash
# Side-by-side arm A vs arm B at one operating point.
#
# Arm B loads a BPF stream parser, which needs CAP_BPF:
#   sudo scripts/smoke_kcm.sh
set -euo pipefail
cd "$(dirname "$0")/.."

CONNS="${CONNS:-32}"
WORKERS="${WORKERS:-4}"
RATE="${RATE:-30000}"
DURATION="${DURATION:-6}"
WARMUP="${WARMUP:-1}"
LARGE_PCT="${LARGE_PCT:-1}"

modprobe kcm 2>/dev/null || true
mkdir -p results

run() {
  local arm="$1" server="$2" port="$3"
  "$server" --port "$port" --workers "$WORKERS" \
            --out "results/smoke_${arm}.server.json" >/dev/null 2>&1 &
  local srv=$!
  sleep 0.7
  ./build/loadgen --port "$port" --conns "$CONNS" --threads 4 \
      --rate "$RATE" --duration "$DURATION" --warmup "$WARMUP" \
      --large-pct "$LARGE_PCT" --arm "$arm" \
      --out "results/smoke_${arm}.json" >/dev/null
  kill -TERM $srv 2>/dev/null || true
  wait $srv 2>/dev/null || true
  sleep 0.5
}

echo "conns=$CONNS workers=$WORKERS rate=$RATE large_pct=$LARGE_PCT duration=${DURATION}s"
echo
run userspace ./build/server_userspace 9400
run kcm       ./build/server_kcm       9401

python3 - <<'PY'
import json
rows = []
for arm in ("userspace", "kcm"):
    r = json.load(open(f"results/smoke_{arm}.json"))
    s = json.load(open(f"results/smoke_{arm}.server.json"))
    l = r["latency"]["small_ol"]
    rows.append((arm, r["sent"], r["recvd"], l["p50_ns"]/1e3, l["p99_ns"]/1e3,
                 l["p999_ns"]/1e3, s["worker_busy_pct_spread"]))

hdr = f"{'arm':<10} {'sent':>8} {'recvd':>8} {'p50 us':>9} {'p99 us':>10} {'p99.9 us':>10} {'busy spread':>12}"
print(hdr); print("-" * len(hdr))
for a, se, re_, p50, p99, p999, sp in rows:
    print(f"{a:<10} {se:>8} {re_:>8} {p50:>9.1f} {p99:>10.1f} {p999:>10.1f} {sp:>12.2f}")
if len(rows) == 2:
    print()
    print(f"p99   improvement: {rows[0][4]/rows[1][4]:.2f}x")
    print(f"p99.9 improvement: {rows[0][5]/rows[1][5]:.2f}x")
PY

# result files land as root when run under sudo
if [[ -n "${SUDO_USER:-}" ]]; then chown -R "$SUDO_USER" results/ 2>/dev/null || true; fi
