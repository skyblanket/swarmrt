#!/bin/bash
# tests/stress/run_stress.sh
#
# 80k-spawn microbench × 20 runs. Asserts >=18 of them print the
# expected "ok 80000" line. Below that threshold is the documented
# arena-slot reuse race (docs/notes/KNOWN_ISSUES.md), which we want
# CI to fail on so regressions are surfaced.
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

RUNS=${SW_STRESS_RUNS:-20}
THRESHOLD=${SW_STRESS_THRESHOLD:-18}
complete=0
crashes=0

if [ -t 1 ]; then
    GREEN=$'\e[32m' ; RED=$'\e[31m' ; DIM=$'\e[2m' ; RESET=$'\e[0m'
else
    GREEN='' ; RED='' ; DIM='' ; RESET=''
fi

echo "=== sw stress: $RUNS x spawn-80k (>=${THRESHOLD} must complete) ==="
for i in $(seq 1 "$RUNS"); do
    out=$("$BIN" 2>/dev/null)
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
echo "completed: ${complete}/${RUNS}"
echo "crashed:   ${crashes}/${RUNS}"

if [ "$complete" -ge "$THRESHOLD" ]; then
    echo "${GREEN}STRESS PASSED${RESET} -- ${complete} >= ${THRESHOLD}"
    exit 0
else
    echo "${RED}STRESS FAILED${RESET} -- ${complete} < ${THRESHOLD}"
    echo "${DIM}see docs/notes/KNOWN_ISSUES.md (R2-#4 -- arena-slot reuse race)${RESET}"
    exit 1
fi
