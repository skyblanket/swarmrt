#!/bin/bash
# tests/stress/run_stress.sh
#
# 80k-spawn microbench × N runs. Pass --variant single to also exercise
# SW_SCHEDULERS=1 — reviewer's round-6 report showed the post-R5-B race
# reproduces even with a single scheduler, so the multi-scheduler-only
# variant doesn't catch it. Defaults: 50 runs, threshold 45 (90%).
#
# Asserts the documented arena/send-path race count stays under
# threshold. The race is documented in docs/notes/KNOWN_ISSUES.md.
#
# NOTE: requires native Linux x86_64 thread scheduling. The race is
# suppressed under:
#   - Docker Desktop on Apple Silicon (runs x86_64 via qemu user-mode)
#   - valgrind (serialises thread interleavings)
#   - any emulated/translated thread schedule
#
# On a native amd64 host the race fires reliably above ~62k spawns;
# below that the runtime is rock-solid. Don't lower N unless you're
# OK with a green CI light that doesn't actually verify anything.

set -u
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"
BIN="$SWARMRT_ROOT/tests/stress/.build/bn"

if [ ! -x "$SWC" ]; then
    echo "error: swc not found at $SWC -- run 'make swc libswarmrt' first" >&2
    exit 2
fi

mkdir -p "$(dirname "$BIN")"

cat > "$SWARMRT_ROOT/tests/stress/bn.sw" << 'EOF'
module Bisect
export [main]

fun child(parent) { send(parent, 'done') ; 'ok' }

fun spawn_n(n) {
    if (n == 0) { 'ok' }
    else { spawn(child(self())) ; spawn_n(n - 1) }
}

fun await_n(n) {
    if (n == 0) { 'ok' }
    else { receive { 'done' -> await_n(n - 1) } }
}

fun main() {
    n = 80000
    spawn_n(n); await_n(n)
    print(f"ok {n}")
}
EOF

"$SWC" build "$SWARMRT_ROOT/tests/stress/bn.sw" -o "$BIN" >/dev/null 2>&1
if [ ! -x "$BIN" ]; then
    echo "error: stress bench failed to compile" >&2
    exit 2
fi

RUNS=${SW_STRESS_RUNS:-50}
THRESHOLD=${SW_STRESS_THRESHOLD:-45}

if [ -t 1 ]; then
    GREEN=$'\e[32m' ; RED=$'\e[31m' ; DIM=$'\e[2m' ; RESET=$'\e[0m'
else
    GREEN='' ; RED='' ; DIM='' ; RESET=''
fi

# Run one variant: a label + env for the bench invocation. We run both
# the default multi-scheduler env and a SW_SCHEDULERS=1 variant because
# the post-R5-B race (R6-A in KNOWN_ISSUES) reproduces with a single
# scheduler too — multi-only wouldn't catch it. Each variant tracks its
# own pass count and fails the script if it crosses the threshold.
run_variant() {
    local label="$1"
    local env_prefix="$2"
    local complete=0
    local crashes=0

    echo "=== sw stress: $RUNS x spawn-80k [$label] (>=${THRESHOLD} must complete) ==="
    for i in $(seq 1 "$RUNS"); do
        out=$(env $env_prefix "$BIN" 2>/dev/null)
        if echo "$out" | grep -q "^ok 80000"; then
            complete=$((complete + 1))
            printf "  ${GREEN}OK${RESET}"
        else
            crashes=$((crashes + 1))
            printf "  ${RED}XX${RESET}"
        fi
    done
    echo ""
    echo ""
    echo "  [$label] completed: ${complete}/${RUNS}, crashed: ${crashes}/${RUNS}"
    if [ "$complete" -ge "$THRESHOLD" ]; then
        echo "  [$label] ${GREEN}PASSED${RESET} (${complete} >= ${THRESHOLD})"
        return 0
    else
        echo "  [$label] ${RED}FAILED${RESET} (${complete} < ${THRESHOLD})"
        return 1
    fi
}

failed=0
if ! run_variant "multi-sched" ""; then
    failed=$((failed + 1))
fi
echo ""
if ! run_variant "single-sched" "SW_SCHEDULERS=1"; then
    failed=$((failed + 1))
fi

echo ""
if [ "$failed" -eq 0 ]; then
    echo "${GREEN}STRESS PASSED${RESET} -- both variants over threshold"
    exit 0
else
    echo "${RED}STRESS FAILED${RESET} -- ${failed} variant(s) under threshold"
    echo "${DIM}see docs/notes/KNOWN_ISSUES.md (R2-#4 / R6-A — spawn-storm race)${RESET}"
    exit 1
fi
