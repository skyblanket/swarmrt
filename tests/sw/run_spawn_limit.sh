#!/bin/bash
# Regression test: when the OS refuses a stack mapping, the node must not crash.
# Processes run on carrier stacks (PROCESS_WEIGHT.md Phase 2), so the mapping
# is refused when a blocked process needs a carrier; that process fails with a
# loud "carrier stack" message and the rest of the node keeps going.
# A failed stack mmap used to be ignored and the half-built process SIGSEGV'd
# (on stock Linux at ~32,700 processes: 2 mappings each, vm.max_map_count
# 65530). `ulimit -v` makes the mmap fail early without needing that limit.
#
# Needs bin/swc (make swc). Exit non-zero if the check fails.
set -u
# macOS has no `ulimit -v` (and no `timeout`); the bug was Linux-only anyway.
if [ "$(uname -s)" != "Linux" ]; then
    echo "SKIP spawn_limit (needs Linux ulimit -v)"
    exit 0
fi
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cat > "$WORK/spawn_many.sw" <<'SW'
module Spawn_many
fun idle() { receive { _o -> idle() } }
fun spawn_n(i, n) { if (i >= n) { 'ok' } else { spawn(idle())  spawn_n(i + 1, n) } }
fun main() { spawn_n(0, 200000)  print("spawned all") }
SW
"$SWC" build "$WORK/spawn_many.sw" -o "$WORK/spawn_many" >/dev/null 2>&1 || { echo "FAIL spawn_limit: build"; exit 1; }
out=$( (ulimit -v 1500000; SW_SCHEDULERS=2 timeout 60 "$WORK/spawn_many") 2>&1); rc=$?
# Carrier failure: only the starved processes fail, so main must still finish.
# Spawn failure (pre-carrier runtimes): the spawner panics, non-zero exit.
if { [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q "carrier stack" &&
     printf '%s' "$out" | grep -q "spawned all"; } ||
   { [ "$rc" -ne 0 ] && [ "$rc" -ne 139 ] && [ "$rc" -ne 124 ] &&
     printf '%s' "$out" | grep -q "spawn failed"; }; then
    echo "PASS spawn_limit (rc=$rc, clean failure)"
    exit 0
fi
echo "FAIL spawn_limit: rc=$rc"
printf '%s\n' "$out" | head -5
exit 1
