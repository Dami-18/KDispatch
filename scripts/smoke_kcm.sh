#!/usr/bin/env bash
# Side-by-side arm A vs arm B at one operating point.
#
# Arm B loads a BPF stream parser, which needs CAP_BPF. Either run the whole
# script under sudo, or grant the binary the capability once:
#
#   sudo scripts/setcap.sh     # then run this script unprivileged
#   sudo scripts/smoke_kcm.sh  # or just run the whole thing as root
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ $EUID -ne 0 ]] && ! getcap build/server_kcm 2>/dev/null | grep -q cap_bpf; then
  echo "server_kcm cannot load its BPF parser as an unprivileged user." >&2
  echo "run 'sudo scripts/setcap.sh', or re-run this script under sudo." >&2
  echo "note: file capabilities are dropped on every rebuild of server_kcm." >&2
  exit 1
fi

CONNS="${CONNS:-32}"
WORKERS="${WORKERS:-4}"
RATE="${RATE:-30000}"
DURATION="${DURATION:-6}"
WARMUP="${WARMUP:-1}"
LARGE_PCT="${LARGE_PCT:-1}"

modprobe kcm 2>/dev/null || true
mkdir -p results

# Under sudo every result file lands owned by root, which then makes later
# unprivileged runs fail to reopen them. Hand them back on the way out,
# including on failure.
if [[ -n "${SUDO_USER:-}" ]]; then
  trap 'chown -R "$SUDO_USER" results/ 2>/dev/null || true' EXIT
fi

run() {
  local arm="$1" server="$2" port="$3"
  local log="results/smoke_${arm}.server.log"

  "$server" --port "$port" --workers "$WORKERS" \
            --out "results/smoke_${arm}.server.json" > "$log" 2>&1 &
  local srv=$!
  # A failure anywhere below must not leave the server holding the port.
  trap 'kill -TERM '"$srv"' 2>/dev/null || true' EXIT
  sleep 0.7

  if ! kill -0 "$srv" 2>/dev/null; then
    echo "$arm server failed to start:" >&2
    sed 's/^/  /' "$log" >&2
    exit 1
  fi

  ./build/loadgen --port "$port" --conns "$CONNS" --threads 4 \
      --rate "$RATE" --duration "$DURATION" --warmup "$WARMUP" \
      --large-pct "$LARGE_PCT" --arm "$arm" \
      --out "results/smoke_${arm}.json" >/dev/null

  kill -TERM $srv 2>/dev/null || true
  wait $srv 2>/dev/null || true
  trap - EXIT
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

