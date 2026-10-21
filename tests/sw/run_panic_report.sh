#!/bin/bash
# Regression gate: `swc run` must print the same panic report as the compiled
# binary (banner, `at FILE:LINE`, call chain) — including across a tail call,
# an imported module, and a spawned worker parked in receive.

set -u
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"
DIR="$SWARMRT_ROOT/tests/sw/.build/panic_report"

if [ ! -x "$SWC" ]; then
    echo "error: swc not found at $SWC — run 'make swc libswarmrt' first" >&2
    exit 2
fi

rm -rf "$DIR"; mkdir -p "$DIR"
cd "$DIR" || exit 2

cat >Helper.sw <<'EOT'
module Helper

fun blow() {
    hd([])
}
EOT
cat >Imp.sw <<'EOT'
module Imp
import Helper

fun main() {
    print("x")
    Helper.blow()
}
EOT
cat >Tail.sw <<'EOT'
module Tail

fun boom() {
    hd([])
}

fun go(n) {
    boom()
}

fun main() {
    go(3)
}
EOT
cat >Fib.sw <<'EOT'
module Fib

fun wait_here() {
    receive { x -> x }
}

fun worker() {
    wait_here()
}

fun main() {
    spawn(worker())
    sleep(50)
    hd([])
}
EOT

fail=0
for m in Imp Tail Fib; do
    "$SWC" run "$m.sw" >"$m.run.out" 2>"$m.run.err"; rc_run=$?
    if ! "$SWC" build "$m.sw" -o "$m.bin" >/dev/null 2>&1; then
        echo "FAIL: swc build $m.sw"; fail=1; continue
    fi
    "./$m.bin" >"$m.bin.out" 2>"$m.bin.err"; rc_bin=$?
    if [ "$rc_run" != 1 ] || [ "$rc_bin" != 1 ]; then
        echo "FAIL: $m exit codes run=$rc_run build=$rc_bin (want 1)"; fail=1
    fi
    if ! grep -q "call chain (innermost first):" "$m.run.err"; then
        echo "FAIL: $m: swc run printed no call chain"; fail=1
    fi
    if ! diff "$m.run.err" "$m.bin.err" >"$m.diff"; then
        echo "FAIL: $m: swc run and compiled panic reports differ"
        sed 's/^/    /' "$m.diff"; fail=1
    fi
done

[ "$fail" = 0 ] && echo "panic-report-gate: PASS"
exit "$fail"
