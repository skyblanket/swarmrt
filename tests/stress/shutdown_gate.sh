#!/usr/bin/env bash
# shutdown_gate.sh — bidirectional gate for graceful shutdown (Phase 4).
#
# Drives the REAL production path: a long-lived sw server (never returns from
# main), a real SIGTERM, the async-signal-safe handler → sw_wait_for_exit on
# the main thread → sw_shutdown_graceful (drain-with-deadline) → hard teardown.
#
# Phase A (quiescent): idle workers parked in receive. SIGTERM must drain
#   INSTANTLY (node already quiescent) and exit 0 WELL BEFORE the 2000ms
#   deadline — proving graceful shutdown doesn't just sleep the whole window.
# Phase B (hung): a busy infinite-loop worker that never quiesces. SIGTERM
#   canNOT drain it, so the deadline must FORCE teardown and the process must
#   still exit — BOUNDED at ~deadline, not hanging forever. This is the
#   bidirectional proof: neuter the deadline break in sw_shutdown_graceful and
#   this phase hangs past the ceiling → the gate FAILs.
#
# Expects bin/shutdown_server prebuilt (make shutdown-gate does this).
set -u
REPO="${SWARMRT_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/../.." && pwd)}"
cd "$REPO" || { echo "shutdown-gate: FAIL (no repo)"; exit 99; }

SRV=bin/shutdown_server
now_ms() { python3 -c 'import time; print(int(time.time()*1000))'; }

# wait_ready <logfile> — poll until the server prints READY (max ~10s)
wait_ready() {
    for _ in $(seq 1 100); do
        grep -q SHUTDOWN_SERVER_READY "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# run_case <mode> <grace_ms> <ceiling_ms> — start server, SIGTERM once ready,
# return "rc took_ms" via globals RC / TOOK; hard-kills if it blows the ceiling.
RC=0; TOOK=0
run_case() {
    local mode="$1" grace="$2" ceiling="$3"
    local out=bin/_sd_${mode}.out err=bin/_sd_${mode}.err
    : > "$out"; : > "$err"
    SW_SHUTDOWN_GRACE_MS="$grace" "$SRV" "$mode" >"$out" 2>"$err" &
    local pid=$!
    if ! wait_ready "$out"; then
        echo "shutdown-gate: FAIL ($mode — server never became ready)"; cat "$err"
        kill -9 "$pid" 2>/dev/null; return 1
    fi
    local t0; t0=$(now_ms)
    kill -TERM "$pid"
    # Watchdog: if it exceeds the ceiling it's HUNG (the bug this gate guards) —
    # hard-kill and fail.
    local wd_secs=$(( ceiling / 1000 + 4 ))   # ceiling + margin, integer seconds
    ( sleep "$wd_secs"; kill -9 "$pid" 2>/dev/null ) &
    local wd=$!
    disown "$wd" 2>/dev/null   # keep the watchdog out of job control (no "Terminated" noise)
    wait "$pid"; RC=$?
    kill "$wd" 2>/dev/null
    local t1; t1=$(now_ms)
    TOOK=$((t1 - t0))
    return 0
}

# --- Phase A: quiescent → instant drain --------------------------------------
run_case idle 2000 2000 || exit 1
if [ "$RC" -ne 0 ]; then
    echo "shutdown-gate: FAIL (idle exit rc=$RC, expected 0)"; cat bin/_sd_idle.err; exit 1
fi
if [ "$TOOK" -ge 1000 ]; then
    echo "shutdown-gate: FAIL (idle took ${TOOK}ms — a quiescent node should drain near-instantly, not near the 2000ms deadline)"; exit 1
fi
if ! grep -q "drained in" bin/_sd_idle.err; then
    echo "shutdown-gate: FAIL (idle — no 'drained in' drain confirmation on stderr)"; cat bin/_sd_idle.err; exit 1
fi
echo "shutdown-gate: phase A OK (quiescent drained + exited in ${TOOK}ms)"

# --- Phase B: hung workload → deadline-bounded teardown -----------------------
run_case busy 800 800 || exit 1
if [ "$RC" -ne 0 ]; then
    echo "shutdown-gate: FAIL (busy exit rc=$RC, expected a clean bounded exit)"; cat bin/_sd_busy.err; exit 1
fi
# Must have WAITED ~the deadline (proves it tried to drain) ...
if [ "$TOOK" -lt 700 ]; then
    echo "shutdown-gate: FAIL (busy exited in ${TOOK}ms — under the 800ms deadline; it should drain up to the deadline)"; exit 1
fi
# ... and must be BOUNDED (proves the deadline broke the never-quiescing loop).
if [ "$TOOK" -ge 3000 ]; then
    echo "shutdown-gate: FAIL (busy took ${TOOK}ms — the deadline did NOT bound the hung workload)"; exit 1
fi
if ! grep -q "forcing teardown" bin/_sd_busy.err; then
    echo "shutdown-gate: FAIL (busy — no 'forcing teardown' on stderr; deadline path not taken)"; cat bin/_sd_busy.err; exit 1
fi
echo "shutdown-gate: phase B OK (hung workload deadline-bounded, exited in ${TOOK}ms)"

rm -f bin/_sd_idle.out bin/_sd_idle.err bin/_sd_busy.out bin/_sd_busy.err
echo "shutdown-gate: PASS (bidirectional — quiescent drains fast, hung workload bounded by deadline)"
exit 0
