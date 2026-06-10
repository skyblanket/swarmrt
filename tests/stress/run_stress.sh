#!/bin/bash
# tests/stress/run_stress.sh
#
# 80k-spawn microbench x N runs. Runs both the default scheduler
# configuration and SW_SCHEDULERS=1 because previous regressions showed up
# in different interleavings across those modes. Defaults: 50 runs per
# variant, strict threshold: every run must complete.
#
# Regression gate for the high-process-count spawn/send/receive path.
#
# NOTE: requires native Linux x86_64 thread scheduling to meaningfully
# exercise the historical failure mode. These environments are smoke tests
# only for this specific class of scheduler race:
#   - Docker Desktop on Apple Silicon (runs x86_64 via qemu user-mode)
#   - valgrind (serialises thread interleavings)
#   - any emulated/translated thread schedule
#
# Keep N high. The original bug only surfaced in high-spawn stress and low
# values can give a green light without exercising the path that matters.

set -u
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"
BIN="$SWARMRT_ROOT/tests/stress/.build/bn"

if [ ! -x "$SWC" ]; then
    echo "error: swc not found at $SWC -- run 'make swc libswarmrt' first" >&2
    exit 2
fi

mkdir -p "$(dirname "$BIN")"

# Print the host limits this gate is sensitive to (O8, Round-7): the
# verdict used to depend SILENTLY on vm.max_map_count — 80k live fiber
# stacks need ~160k VMAs, over the stock-Linux 65530 default but under
# Ubuntu-24.04's ~1M and GitHub-runner values, so the same commit passed
# on CI and crashed locally. The backedge-yield fix removed that failure
# mode; printing the limits keeps any future env-dependence diagnosable.
if [ "$(uname)" = "Linux" ]; then
    echo "host limits: vm.max_map_count=$(sysctl -n vm.max_map_count 2>/dev/null || echo '?')" \
         "nproc=$(nproc) ulimit-v=$(ulimit -v) ulimit-n=$(ulimit -n)"
fi

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
THRESHOLD=${SW_STRESS_THRESHOLD:-$RUNS}

if [ -t 1 ]; then
    GREEN=$'\e[32m' ; RED=$'\e[31m' ; DIM=$'\e[2m' ; RESET=$'\e[0m'
else
    GREEN='' ; RED='' ; DIM='' ; RESET=''
fi

# Run one variant: a label + env for the bench invocation. Each variant
# tracks its own pass count and fails the script if it misses the required
# completion threshold.
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
    echo "${GREEN}STRESS PASSED${RESET} -- both variants met completion threshold"
    exit 0
else
    echo "${RED}STRESS FAILED${RESET} -- ${failed} variant(s) under threshold"
    echo "${DIM}high-spawn regression: rerun on native Linux x86_64 and capture crash output${RESET}"
    exit 1
fi
