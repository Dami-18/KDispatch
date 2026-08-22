#!/usr/bin/env bash
# Throughput-under-SLO: the highest offered load at which small-message p99
# stays under a target. This is the metric Rakaia reports, so it is the one
# that makes these results comparable in kind to the paper's.
#
# Binary search on offered rate. A rate "passes" only if BOTH hold:
#   - small-message p99 (measured from the scheduled send time) <= SLO_US
#   - the generator actually kept its schedule (achieved >= 95% of offered),
#     otherwise we would be measuring the load generator, not the server.
#
#   scripts/slo_ladder.sh userspace
#   SLO_US=500 CONNS=32 WORKERS=4 scripts/slo_ladder.sh module
set -euo pipefail
cd "$(dirname "$0")/.."

ARM="${1:-${ARM:-userspace}}"
SLO_US="${SLO_US:-500}"
CONNS="${CONNS:-32}"
WORKERS="${WORKERS:-4}"
DURATION="${DURATION:-4}"
WARMUP="${WARMUP:-1}"
LARGE_PCT="${LARGE_PCT:-1}"
LARGE_SIZE="${LARGE_SIZE:-128}"
LARGE_WORK_US="${LARGE_WORK_US:-2000}"
PORT="${PORT:-9970}"
OUTDIR="${OUTDIR:-results}"
LO="${LO:-2000}"
HI="${HI:-400000}"
STEPS="${STEPS:-7}"

case "$ARM" in
  userspace) SERVER=./build/server_userspace ;;
  kcm)       SERVER=./build/server_kcm ;;
  module)    SERVER=./build/server_module ;;
  *) echo "unknown arm: $ARM" >&2; exit 2 ;;
esac
[[ -x "$SERVER" ]] || { echo "missing $SERVER" >&2; exit 1; }
mkdir -p "$OUTDIR"

# Runs one rate; echoes "PASS <p99us> <achieved>" or "FAIL <p99us> <achieved>".
try_rate() {
  local rate=$1
  local tag="slo_${ARM}_c${CONNS}_w${WORKERS}_rate${rate}"
  "$SERVER" --port "$PORT" --workers "$WORKERS" \
            --out "$OUTDIR/${tag}.server.json" > "$OUTDIR/${tag}.server.log" 2>&1 &
  local srv=$!
  trap 'kill -TERM '"$srv"' 2>/dev/null || true' EXIT
  sleep 0.6
  if ! kill -0 "$srv" 2>/dev/null; then
    echo "server failed to start:" >&2; sed 's/^/  /' "$OUTDIR/${tag}.server.log" >&2; exit 1
  fi

  local threads=$(( CONNS < 8 ? CONNS : 8 ))
  timeout 60 ./build/loadgen --port "$PORT" --conns "$CONNS" --threads "$threads" \
      --rate "$rate" --duration "$DURATION" --warmup "$WARMUP" \
      --large-pct "$LARGE_PCT" --large-size "$LARGE_SIZE" \
      --large-work-us "$LARGE_WORK_US" --arm "$ARM" \
      --out "$OUTDIR/${tag}.json" >/dev/null 2>&1 || true

  kill -TERM $srv 2>/dev/null || true
  wait $srv 2>/dev/null || true
  trap - EXIT
  sleep 0.4

  python3 - "$OUTDIR/${tag}.json" "$SLO_US" <<'PY'
import json, sys
try:
    r = json.load(open(sys.argv[1]))
except Exception:
    print("FAIL 0 0"); raise SystemExit
p99 = r["latency"]["small_ol"]["p99_ns"] / 1e3
kept = r["achieved_rate"] >= 0.95 * r["offered_rate"]
lost = r["sent"] != r["recvd"]
ok = (p99 <= float(sys.argv[2])) and kept and not lost
print(f"{'PASS' if ok else 'FAIL'} {p99:.1f} {r['achieved_rate']:.0f}")
PY
}

echo "arm=$ARM slo=${SLO_US}us conns=$CONNS workers=$WORKERS large_pct=$LARGE_PCT"
lo=$LO; hi=$HI; best=0; best_p99=0
for ((i=0; i<STEPS; i++)); do
  mid=$(( (lo + hi) / 2 ))
  read -r verdict p99 achieved <<<"$(try_rate $mid)"
  printf "  rate=%-7s p99=%-9s achieved=%-8s %s\n" "$mid" "${p99}us" "$achieved" "$verdict"
  if [[ "$verdict" == "PASS" ]]; then
    best=$mid; best_p99=$p99; lo=$(( mid + 1 ))
  else
    hi=$(( mid - 1 ))
  fi
  (( lo > hi )) && break
done

echo "  => throughput-under-SLO: ${best} msg/s (p99 ${best_p99}us at that rate)"
python3 - <<PY > "$OUTDIR/slo_${ARM}_c${CONNS}_w${WORKERS}.result.json"
import json
print(json.dumps({"arm": "$ARM", "slo_us": $SLO_US, "conns": $CONNS,
                  "workers": $WORKERS, "large_pct": $LARGE_PCT,
                  "throughput_under_slo": $best, "p99_at_best_us": $best_p99}, indent=2))
PY
