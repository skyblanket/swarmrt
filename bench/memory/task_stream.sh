#!/usr/bin/env bash
# bench/memory/task_stream.sh — per-process memory of Std.task_stream workers
# that are all waiting (PROCESS_WEIGHT.md Phase 2e): "llm" waits in
# llm_complete on bench/rollout's mock server (it never answers EP ids
# % 100 == 7 on turn 0), "idle" waits in a receive. Prints RSS, mappings and
# threads at N=1000 and N=10000; the per-process cost is the slope.
#
#   bash bench/memory/task_stream.sh            # needs python3 + aiohttp
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$(mktemp -d)"
PORT="${BENCH_PORT:-18577}"
python3 "$ROOT/bench/rollout/mock_server.py" "$PORT" >"$WORK/mock.log" 2>&1 &
MOCK=$!
trap 'kill $MOCK 2>/dev/null || true; wait $MOCK 2>/dev/null || true; rm -rf "$WORK"' EXIT
"$ROOT/bin/swc" build "$HERE/task_stream.sw" -o "$WORK/ts" >"$WORK/build.log" 2>&1 || { cat "$WORK/build.log" >&2; exit 1; }
ready=0
for _ in $(seq 50); do curl -sf "127.0.0.1:$PORT/stats" >/dev/null && { ready=1; break; }; sleep 0.1; done
if [ "$ready" != 1 ]; then echo "error: mock server did not start" >&2; cat "$WORK/mock.log" >&2; exit 1; fi
for mode in idle llm; do
    for n in ${NS:-1000 10000}; do
        MODE=$mode N=$n URL="http://127.0.0.1:$PORT/v1/chat/completions" SW_DEADLOCK_DETECT=0 \
            "$WORK/ts" >/dev/null 2>&1 &
        P=$!
        sleep "${SETTLE:-8}"
        echo "{\"mode\":\"$mode\",\"n\":$n,\"rss_kb\":$(awk '/VmRSS/{print $2}' /proc/$P/status),\"maps\":$(wc -l < /proc/$P/maps),\"threads\":$(awk '/Threads/{print $2}' /proc/$P/status)}"
        kill -9 $P 2>/dev/null; wait $P 2>/dev/null || true
    done
done
exit 0
