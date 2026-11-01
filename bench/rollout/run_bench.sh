#!/usr/bin/env bash
# Run the rollout benchmark: both runners, three scenarios. Writes a summary
# to stdout and per-run files to $OUT_DIR.
#
#   bash run_bench.sh [N] [CONC]
#   MOCK=sw bash run_bench.sh ...    # serve the model API from mock_server.sw
#   RUN_PY=0 bash run_bench.sh ...   # SwarmRT runner only (no Python baseline)
#   SCENARIOS="api resume" ...       # a subset (default: clean api runaway resume)
#   KILL_AT=8 ...                    # resume: seconds before the SIGKILL (default 20)
#
# The CI gate (make bench-rollout-gate, ci_gate.py) runs the SwarmRT runner on
# the sw mock, 500 episodes, and asserts outcomes, orphans and duplicates.
#
# Scenarios:
#   api      model faults (429, 500, a hung request), a hung tool that leaves a
#            grandchild, a grader that raises
#   runaway  api + a grader that never returns (CPU loop) on 1% of episodes
#   clean    no faults anywhere: throughput, CPU and memory
#   resume   api, the runner is killed with SIGKILL partway through, then
#            restarted on the same results file
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
N="${1:-10000}"
CONC="${2:-128}"
PORT="${BENCH_PORT:-18500}"
OUT_DIR="${OUT_DIR:-$here/out}"
RUN_TIMEOUT="${RUN_TIMEOUT:-900}"
mkdir -p "$OUT_DIR"
export BENCH_URL="http://127.0.0.1:$PORT/v1/chat/completions"
export SW_DEADLOCK_DETECT=0

"$here/../../bin/swc" build "$here/swarmrt_runner.sw" -o "$OUT_DIR/swarmrt_runner" >/dev/null || exit 1

# MOCK=sw serves the model API from mock_server.sw instead (dogfooding
# SwarmRT's HTTP server). The Python mock is the neutral default.
if [ "${MOCK:-py}" = sw ]; then
    "$here/../../bin/swc" build "$here/mock_server.sw" -o "$OUT_DIR/mock_server" >/dev/null || exit 1
    PORT="$PORT" "$OUT_DIR/mock_server" > "$OUT_DIR/mock.log" 2>&1 &
else
    python3 "$here/mock_server.py" "$PORT" > "$OUT_DIR/mock.log" 2>&1 &
fi
MOCK_PID=$!
trap 'kill $MOCK_PID 2>/dev/null' EXIT
for _ in $(seq 50); do curl -sf "127.0.0.1:$PORT/stats" >/dev/null && break; sleep 0.1; done

cleanup_strays() { pkill -f '^sleep 300$' 2>/dev/null; pkill -f "^curl .*127.0.0.1:$PORT" 2>/dev/null; true; }
# pgrep -c is GNU-only; count lines instead (macOS too).
strays() { echo "$(pgrep -f '^sleep 300$' | wc -l | tr -d ' ') $(pgrep -f "^curl .*127.0.0.1:$PORT" | wc -l | tr -d ' ')"; }

# Portable pieces: macOS has no `timeout`, no `date +%N` and a BSD
# /usr/bin/time without -f; minimal Linux images may have no /usr/bin/time.
now() { python3 -c 'import time; print("%.3f" % time.time())'; }
if command -v timeout >/dev/null 2>&1; then run_to() { timeout -s KILL "$@"; }
elif command -v gtimeout >/dev/null 2>&1; then run_to() { gtimeout -s KILL "$@"; }
else run_to() { local t="$1"; shift; perl -e 'alarm shift; exec @ARGV' "$t" "$@"; }
fi
if /usr/bin/time -f "%M" true >/dev/null 2>&1; then
    timed() { local tf="$1"; shift; /usr/bin/time -f "%M %U %S" -o "$tf" "$@"; }
else
    timed() { local tf="$1"; shift; : > "$tf"; "$@"; }
fi

# run <name> <scenario> <command...>: one timed run; prints a JSON summary line.
run() {
    local name="$1" scenario="$2"; shift 2
    local res="$OUT_DIR/$name.jsonl" tf="$OUT_DIR/$name.time"
    rm -f "$res"; cleanup_strays
    local f=true; [ "$scenario" = clean ] && f=false
    curl -sf -X POST -H 'Content-Type: application/json' -d "{\"faults\": $f}" "127.0.0.1:$PORT/reset" >/dev/null
    local t0; t0=$(now)
    BENCH_N="$N" BENCH_CONC="$CONC" BENCH_OUT="$res" BENCH_SCENARIO="$scenario" \
        timed "$tf" run_to "$RUN_TIMEOUT" "$@" > "$OUT_DIR/$name.log" 2>&1
    local rc=$?
    local wall; wall=$(echo "$(now) - $t0" | bc)
    sleep 1
    read -r orphan_sleep orphan_curl <<< "$(strays)"
    local rss cpu_u cpu_s; read -r rss cpu_u cpu_s <<< "$(tail -1 "$tf" 2>/dev/null)"
    echo "{\"run\": \"$name\", \"rc\": $rc, \"wall_s\": $(printf %.1f "$wall"), \"peak_rss_mb\": $(( ${rss:-0} / 1024 )), \"cpu_s\": $(echo "${cpu_u:-0} + ${cpu_s:-0}" | bc), \"orphans_after\": {\"sleep\": $orphan_sleep, \"curl\": $orphan_curl}, \"result\": $(python3 "$here/analyze.py" "$res" "$N" "$scenario"), \"server\": $(curl -s "127.0.0.1:$PORT/stats")}"
}

# resume <name> <command...>: SIGKILL the runner at KILL_AT seconds, restart it.
resume() {
    local name="$1"; shift
    local res="$OUT_DIR/$name.jsonl"
    rm -f "$res"; cleanup_strays
    curl -sf -X POST -H 'Content-Type: application/json' -d '{"faults": true}' "127.0.0.1:$PORT/reset" >/dev/null
    BENCH_N="$N" BENCH_CONC="$CONC" BENCH_OUT="$res" BENCH_SCENARIO=api "$@" > "$OUT_DIR/$name.1.log" 2>&1 &
    local pid=$!
    sleep "${KILL_AT:-20}"; kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
    local before; before=$(wc -l < "$res" | tr -d ' ')
    # Tools in flight when the runner is SIGKILLed outlive it (no runner is
    # left to kill their process groups). Count them apart, then clear them,
    # so orphans_after measures only the restarted run.
    sleep 1
    local kill_sleep kill_curl; read -r kill_sleep kill_curl <<< "$(strays)"
    cleanup_strays
    local t0; t0=$(now)
    BENCH_N="$N" BENCH_CONC="$CONC" BENCH_OUT="$res" BENCH_SCENARIO=api \
        run_to "$RUN_TIMEOUT" "$@" > "$OUT_DIR/$name.2.log" 2>&1
    local rc=$?
    local wall; wall=$(echo "$(now) - $t0" | bc)
    sleep 1
    read -r orphan_sleep orphan_curl <<< "$(strays)"
    echo "{\"run\": \"$name\", \"rc\": $rc, \"rows_before_kill\": $before, \"resume_wall_s\": $(printf %.1f "$wall"), \"orphans_from_kill\": {\"sleep\": $kill_sleep, \"curl\": $kill_curl}, \"orphans_after\": {\"sleep\": $orphan_sleep, \"curl\": $orphan_curl}, \"result\": $(python3 "$here/analyze.py" "$res" "$N" api), \"server\": $(curl -s "127.0.0.1:$PORT/stats")}"
}

SW=("$OUT_DIR/swarmrt_runner")
PY=(python3 "$here/python_runner.py")

RUN_PY="${RUN_PY:-1}"
for sc in ${SCENARIOS:-clean api runaway resume}; do
    case "$sc" in
        resume) resume swarmrt_resume "${SW[@]}"
                [ "$RUN_PY" = 1 ] && resume python_resume "${PY[@]}" ;;
        clean|api|runaway)
                run "swarmrt_$sc" "$sc" "${SW[@]}"
                [ "$RUN_PY" = 1 ] && run "python_$sc" "$sc" "${PY[@]}" ;;
        *) echo "run_bench.sh: unknown scenario '$sc'" >&2; exit 2 ;;
    esac
done
cleanup_strays
