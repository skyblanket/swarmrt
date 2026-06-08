#!/usr/bin/env bash
# gc_slope.sh <probe.sw> <low> <high> <budget_mb> [tag] — memory-slope gate.
#
# Compiles <probe.sw> once, runs it at a LOW and a HIGH job/message count (fixed
# concurrency / depth lives in the probe), measures PEAK RSS of each, and asserts
# the post-warmup growth (high - low) stays under <budget_mb> MB. The probe leaks
# proportionally to cumulative work on current main (GC v1 copies escaped values to
# the global heap and never reclaims them), so a tight budget FAILS now and must
# PASS after Ownership v2.
#
#   VERDICT: PASS   growth under budget (near-flat)
#   VERDICT: FAIL   growth over budget (unbounded slope) — the leak this gate guards
#
# No hardcoded paths. Honors SW_SCHEDULERS / SW_GC_OFF passed in the environment.
set -u
REPO="${SWARMRT_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)}"
cd "$REPO" || { echo "VERDICT: FAIL (no repo at $REPO)"; exit 99; }

SW="${1:?usage: gc_slope.sh <probe.sw> <low> <high> <budget_mb> [tag]}"
LOW="${2:?need low count}"; HIGH="${3:?need high count}"; BUDGET="${4:?need budget_mb}"
TAG="${5:-$$}"; bin="/tmp/gcslope_${TAG}"

# Portable peak-RSS (KB) of a command: prefer GNU time -v, else BSD/macOS time -l.
peak_rss_kb() {
  local out
  if command -v gtime >/dev/null 2>&1; then
    out=$(gtime -v "$@" 2>&1)
    echo "$out" | awk -F': ' '/Maximum resident set size/{print $2; found=1} END{if(!found) print -1}'
  else
    # macOS /usr/bin/time -l prints "<bytes>  maximum resident set size"
    out=$(/usr/bin/time -l "$@" 2>&1)
    echo "$out" | awk '/maximum resident set size/{print int($1/1024)}'
  fi
}

./bin/swc build "$SW" -o "$bin" >/dev/null 2>"${bin}.err" || {
  echo "VERDICT: FAIL (compile)"; grep -iE "error|cannot" "${bin}.err" | grep -v writable | head; exit 1; }

# Run each twice and take the min RSS (reduces transient-spike noise from other load).
run_min() {
  local count="$1" a b
  a=$(SW_RUNTIME_QUIET=1 peak_rss_kb "$bin" "$count")
  b=$(SW_RUNTIME_QUIET=1 peak_rss_kb "$bin" "$count")
  [ "$a" -lt "$b" ] 2>/dev/null && echo "$a" || echo "$b"
}

LOW_KB=$(run_min "$LOW"); HIGH_KB=$(run_min "$HIGH")
rm -f "$bin" "${bin}.err"

if ! [ "$LOW_KB" -gt 0 ] 2>/dev/null || ! [ "$HIGH_KB" -gt 0 ] 2>/dev/null; then
  echo "VERDICT: FAIL (could not measure RSS: low=$LOW_KB high=$HIGH_KB)"; exit 1
fi

GROWTH_KB=$(( HIGH_KB - LOW_KB ))
BUDGET_KB=$(( BUDGET * 1024 ))
LOW_MB=$(( LOW_KB / 1024 )); HIGH_MB=$(( HIGH_KB / 1024 )); GROWTH_MB=$(( GROWTH_KB / 1024 ))
PERJOB_KB=$(awk -v g="$GROWTH_KB" -v l="$LOW" -v h="$HIGH" 'BEGIN{d=h-l; printf "%.2f", (d>0? g/d : 0)}')

echo "slope[$(basename "$SW")]: ${LOW} jobs -> ${LOW_MB} MB, ${HIGH} jobs -> ${HIGH_MB} MB | growth ${GROWTH_MB} MB (~${PERJOB_KB} KB/job) | budget ${BUDGET} MB"
if [ "$GROWTH_KB" -le "$BUDGET_KB" ]; then echo "VERDICT: PASS"; exit 0
else echo "VERDICT: FAIL"; exit 1; fi
