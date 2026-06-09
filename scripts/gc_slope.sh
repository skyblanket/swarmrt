#!/usr/bin/env bash
# gc_slope.sh <probe.sw> <low> <high> <budget_mb> [tag] — memory-slope gate.
#
# Compiles <probe.sw> once, runs it at a LOW and a HIGH job/message count (fixed
# concurrency / depth lives in the probe), measures PEAK RSS of each, and asserts
# the post-warmup growth (high - low) stays under <budget_mb> MB.
#
# HONEST: a run only counts if the program EXITS 0 and prints PROBE_OK. A probe
# that crashes / sys_exit(1)s / never finishes FAILS the gate (it does not get to
# report a flat slope). If peak RSS cannot be measured at all, the gate FAILS
# (never silently passes).
#
#   VERDICT: PASS   both runs clean AND growth under budget (near-flat)
#   VERDICT: FAIL   a run crashed / lacked PROBE_OK, RSS unmeasurable, or slope > budget
#
# No hardcoded paths. Honors SW_SCHEDULERS / SW_GC_OFF passed in the environment.
set -u
REPO="${SWARMRT_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)}"
cd "$REPO" || { echo "VERDICT: FAIL (no repo at $REPO)"; exit 99; }

SW="${1:?usage: gc_slope.sh <probe.sw> <low> <high> <budget_mb> [tag]}"
LOW="${2:?need low count}"; HIGH="${3:?need high count}"; BUDGET="${4:?need budget_mb}"
TAG="${5:-$$}"; bin="/tmp/gcslope_${TAG}"

# Pick a peak-RSS timer ONCE, robustly: GNU `time -v` (stock Linux, in the `time`
# pkg), then BSD `time -l` (macOS), then gtime (coreutils). Empty TIMER = none.
TIMER=""
if /usr/bin/time -v true >/dev/null 2>&1; then TIMER="gnu"
elif /usr/bin/time -l true >/dev/null 2>&1; then TIMER="bsd"
elif command -v gtime >/dev/null 2>&1; then TIMER="gtime"; fi

# Run the probe at <count>; echo its peak RSS in KB on a clean run (exit 0 +
# PROBE_OK), else echo "FAIL:<reason>". RSS is parsed per the detected timer.
measure_kb() {
  local count="$1" out rc rss
  case "$TIMER" in
    gnu)   out=$(SW_RUNTIME_QUIET=1 /usr/bin/time -v "$bin" "$count" 2>&1); rc=$?
           rss=$(printf '%s\n' "$out" | awk -F': ' '/Maximum resident set size/{print $2}') ;;
    bsd)   out=$(SW_RUNTIME_QUIET=1 /usr/bin/time -l "$bin" "$count" 2>&1); rc=$?
           rss=$(printf '%s\n' "$out" | awk '/maximum resident set size/{print int($1/1024)}') ;;
    gtime) out=$(SW_RUNTIME_QUIET=1 gtime -v "$bin" "$count" 2>&1); rc=$?
           rss=$(printf '%s\n' "$out" | awk -F': ' '/Maximum resident set size/{print $2}') ;;
    *)     echo "FAIL:no-rss-timer (install GNU 'time')"; return ;;
  esac
  if [ "$rc" -ne 0 ]; then echo "FAIL:exit=$rc"; return; fi
  if ! printf '%s\n' "$out" | grep -q "PROBE_OK"; then echo "FAIL:no-PROBE_OK"; return; fi
  if ! [ "${rss:-0}" -gt 0 ] 2>/dev/null; then echo "FAIL:rss-unparsed"; return; fi
  echo "$rss"
}

# Min of two clean runs (reduces transient-spike noise). FAIL propagates.
run_min() {
  local a b
  a=$(measure_kb "$1"); case "$a" in FAIL:*) echo "$a"; return;; esac
  b=$(measure_kb "$1"); case "$b" in FAIL:*) echo "$b"; return;; esac
  [ "$a" -lt "$b" ] && echo "$a" || echo "$b"
}

./bin/swc build "$SW" -o "$bin" >/dev/null 2>"${bin}.err" || {
  echo "VERDICT: FAIL (compile)"; grep -iE "error|cannot" "${bin}.err" | grep -v writable | head; exit 1; }

LOW_KB=$(run_min "$LOW"); HIGH_KB=$(run_min "$HIGH")
rm -f "$bin" "${bin}.err"

case "$LOW_KB" in FAIL:*) echo "slope[$(basename "$SW")]: low run bad ($LOW_KB)"; echo "VERDICT: FAIL"; exit 1;; esac
case "$HIGH_KB" in FAIL:*) echo "slope[$(basename "$SW")]: high run bad ($HIGH_KB)"; echo "VERDICT: FAIL"; exit 1;; esac

GROWTH_KB=$(( HIGH_KB - LOW_KB ))
BUDGET_KB=$(( BUDGET * 1024 ))
LOW_MB=$(( LOW_KB / 1024 )); HIGH_MB=$(( HIGH_KB / 1024 )); GROWTH_MB=$(( GROWTH_KB / 1024 ))
PERJOB_KB=$(awk -v g="$GROWTH_KB" -v l="$LOW" -v h="$HIGH" 'BEGIN{d=h-l; printf "%.2f", (d>0? g/d : 0)}')

echo "slope[$(basename "$SW")]: ${LOW} jobs -> ${LOW_MB} MB, ${HIGH} jobs -> ${HIGH_MB} MB | growth ${GROWTH_MB} MB (~${PERJOB_KB} KB/job) | budget ${BUDGET} MB"
if [ "$GROWTH_KB" -le "$BUDGET_KB" ]; then echo "VERDICT: PASS"; exit 0
else echo "VERDICT: FAIL"; exit 1; fi
