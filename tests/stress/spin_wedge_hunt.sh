#!/bin/bash
# spin_wedge_hunt.sh — reproducer + regression gate for the spin-gated
# scheduler deadlock (June 2026, Round-7 continuation).
#
# With the idle-loop spin enabled (SW_SPIN_US>0), a depth-1
# cross-scheduler ping-pong with ~32KB payloads (the gc-slope message
# probe) deadlocks in ~15% of runs: both fibers parked WAITING, every
# runq empty, ZERO enqueues from then on (verify with SW_SCHED_TRACE=1:
# enq=0, park_to≈6600/s while wedged). With spin off it was 0/60.
# The publish-idle-then-repoll guard does not close it, so the loss is
# upstream of the park: runq push vs spinning-pick, or the receive
# waiting-flag handoff. Root-cause via TSan (roadmap Phase 2.3/2.4).
#
# Usage:
#   ./tests/stress/spin_wedge_hunt.sh            # 40 runs, SW_SPIN_US=30
#   SW_SPIN_US=100 RUNS=100 ./tests/...          # wider window, more runs
#
# Exits 1 on the first wedge (run exceeding TIMEOUT_S — healthy runs
# take ~0.1-0.5s), 0 if all runs complete. Once the race is fixed, wire
# this into the stress gate with SW_SPIN_US=30 and a high RUNS count.

set -u
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"
BIN="$SWARMRT_ROOT/tests/stress/.build/spin_wedge_probe"
RUNS="${RUNS:-40}"
TIMEOUT_S="${TIMEOUT_S:-20}"
export SW_SPIN_US="${SW_SPIN_US:-30}"

mkdir -p "$(dirname "$BIN")"
"$SWC" build "$SWARMRT_ROOT/tests/gc/slope_message.sw" -o "$BIN" >/dev/null 2>&1 || {
    echo "error: probe failed to compile"; exit 2; }

echo "spin_wedge_hunt: $RUNS runs of depth-1 ping-pong (4000 msgs), SW_SPIN_US=$SW_SPIN_US"
for i in $(seq 1 "$RUNS"); do
    if ! SW_QUIET=1 timeout "$TIMEOUT_S" "$BIN" 4000 >/dev/null 2>&1; then
        echo "WEDGE on run $i (exceeded ${TIMEOUT_S}s — healthy is ~0.1-0.5s)"
        echo "spin_wedge_hunt: FAIL"
        exit 1
    fi
done
echo "spin_wedge_hunt: PASS ($RUNS/$RUNS completed)"
exit 0
