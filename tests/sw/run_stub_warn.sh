#!/bin/bash
# Regression gate: under `swc run`, each stubbed scheduler-only builtin must
# print its own note on stderr. A single shared "already warned" flag once
# silenced every stub after the first, so a test could pass on nil unnoticed.
#
# The fixture calls two different stubs twice each; we expect exactly one
# note per name.

set -u
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"
BUILD_DIR="$SWARMRT_ROOT/tests/sw/.build/stub_warn"

if [ ! -x "$SWC" ]; then
    echo "error: swc not found at $SWC — run 'make swc libswarmrt' first" >&2
    exit 2
fi

mkdir -p "$BUILD_DIR"
fixture="$BUILD_DIR/stub_warn_fixture.sw"
errlog="$BUILD_DIR/stub_warn_fixture.err"

cat >"$fixture" <<'EOF'
module Stub_warn_fixture

fun main() {
    a = tool_list()
    b = ws_close(0)
    c = tool_list()
    d = ws_close(0)
    print(to_string(a) ++ " " ++ to_string(b) ++ " " ++ to_string(c) ++ " " ++ to_string(d))
}
EOF

if ! "$SWC" run "$fixture" >/dev/null 2>"$errlog"; then
    echo "FAIL: swc run exited non-zero on $fixture" >&2
    sed 's/^/    /' "$errlog" >&2
    exit 1
fi

fail=0
for name in tool_list ws_close; do
    count=$(grep -c "note: '$name' " "$errlog")
    if [ "$count" -ne 1 ]; then
        echo "FAIL: expected exactly 1 note for '$name' on stderr, got $count" >&2
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "--- stderr ---" >&2
    sed 's/^/    /' "$errlog" >&2
    exit 1
fi

echo "OK           stub_warn — one note per stubbed builtin name"
