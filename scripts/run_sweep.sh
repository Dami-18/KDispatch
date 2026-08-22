#!/usr/bin/env bash
# Sweep one axis for one arm, dropping a JSON per run into results/.
#
#   scripts/run_sweep.sh userspace large_pct
#   scripts/run_sweep.sh userspace workers
#   SWEEP=conns VALUES="1 4 16 64" REPEATS=3 scripts/run_sweep.sh userspace
#
# Axes:
#   large_pct  fraction of large messages   -- primary HOL-blocking curve
#   workers    server thread count          -- shows threads only dilute HOL blocking
#   conns      connection count             -- control axis (expected flat for arm A)
#   rate       offered load                 -- for throughput-under-SLO ladders
set -euo pipefail
cd "$(dirname "$0")/.."

ARM="${1:-${ARM:-userspace}}"
SWEEP="${2:-${SWEEP:-large_pct}}"
REPEATS="${REPEATS:-3}"
DURATION="${DURATION:-30}"
WARMUP="${WARMUP:-5}"
PORT="${PORT:-9000}"
OUTDIR="${OUTDIR:-results}"

# Fixed operating point; the swept axis overrides its own variable below.
CONNS="${CONNS:-32}"
WORKERS="${WORKERS:-4}"
RATE="${RATE:-30000}"
LARGE_PCT="${LARGE_PCT:-1}"

case "$SWEEP" in
  large_pct) VALUES="${VALUES:-0.1 0.25 0.5 1 2 4}" ;;
  workers)   VALUES="${VALUES:-1 2 4 8 16}" ;;
  conns)     VALUES="${VALUES:-1 2 4 8 16 32 64 128}" ;;
  rate)      VALUES="${VALUES:-10000 20000 30000 50000 80000 120000}" ;;
  *) echo "unknown sweep axis: $SWEEP" >&2; exit 2 ;;
esac

case "$ARM" in
  userspace) SERVER=./build/server_userspace ;;
  kcm)       SERVER=./build/server_kcm ;;
  module)    SERVER=./build/server_module ;;
  *) echo "unknown arm: $ARM" >&2; exit 2 ;;
esac
[[ -x "$SERVER" ]] || { echo "missing $SERVER -- build it first" >&2; exit 1; }

mkdir -p "$OUTDIR"
echo "arm=$ARM sweep=$SWEEP values=[$VALUES] repeats=$REPEATS duration=${DURATION}s"
echo "fixed: conns=$CONNS workers=$WORKERS rate=$RATE large_pct=$LARGE_PCT"

for v in $VALUES; do
  c=$CONNS; w=$WORKERS; r=$RATE; lp=$LARGE_PCT
  case "$SWEEP" in
    large_pct) lp=$v ;;
    workers)   w=$v ;;
    conns)     c=$v ;;
    rate)      r=$v ;;
  esac

  for i in $(seq 1 "$REPEATS"); do
    tag="${ARM}_${SWEEP}${v}_c${c}_w${w}_r${r}_lp${lp}_run${i}"
    echo "  -> $tag"

    "$SERVER" --port "$PORT" --workers "$w" \
              --out "$OUTDIR/${tag}.server.json" >/dev/null 2>&1 &
    srv=$!
    trap 'kill -TERM $srv 2>/dev/null || true' EXIT
    sleep 0.5

    threads=$(( c < 4 ? c : 4 ))
    ./build/loadgen --port "$PORT" --conns "$c" --threads "$threads" \
        --rate "$r" --duration "$DURATION" --warmup "$WARMUP" \
        --large-pct "$lp" --arm "$ARM" --out "$OUTDIR/${tag}.json" >/dev/null

    kill -TERM $srv 2>/dev/null || true
    wait $srv 2>/dev/null || true
    trap - EXIT
    sleep 0.5   # let the port drain
  done
done
echo "done -- $(ls -1 "$OUTDIR"/*.json 2>/dev/null | wc -l) json files in $OUTDIR/"
