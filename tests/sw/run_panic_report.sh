#!/bin/bash
# Regression gate: under `swc run`, a panic in a spawned child must print the
# same loud report as a compiled binary: `panic: <msg>`, `at FILE:LINE`, and a
# call chain naming the function that panicked and its caller.

set -u
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"
BUILD_DIR="$SWARMRT_ROOT/tests/sw/.build/panic_report"

if [ ! -x "$SWC" ]; then
    echo "error: swc not found at $SWC — run 'make swc libswarmrt' first" >&2
    exit 2
fi

mkdir -p "$BUILD_DIR"
fixture="$BUILD_DIR/panic_report_fixture.sw"
errlog="$BUILD_DIR/panic_report_fixture.err"

cat >"$fixture" <<'SW'
module Panic_report_fixture

export [main]

fun worker(n) {
    receive {
        'inc' -> worker(n + 1)
        other -> panic("bad message " ++ other)
    }
}

fun main() {
    p = spawn(fn() { worker(0) })
    send(p, 'bad')
    receive { after 200 -> print("done") }
}
SW

"$SWC" run "$fixture" >/dev/null 2>"$errlog"
fail=0
for pat in 'panic.*: bad message bad' 'at .*panic_report_fixture.sw:8' \
           'call chain' '\[0\] Panic_report_fixture.worker at src/Panic_report_fixture.sw:8' \
           '\[1\] Panic_report_fixture.<lambda>'; do
    if ! grep -q "$pat" "$errlog"; then
        echo "FAIL: swc run stderr missing /$pat/" >&2
        fail=1
    fi
done
if [ $fail -ne 0 ]; then sed 's/^/    /' "$errlog" >&2; exit 1; fi
echo "OK           run_panic_report — child panic reported under swc run"
