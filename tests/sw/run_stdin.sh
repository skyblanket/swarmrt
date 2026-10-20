#!/bin/bash
# Regression test: swc must read source from non-seekable input (pipe,
# /dev/stdin, process substitution). ftell returns -1 on a pipe, so the old
# fixed-size reader overflowed its buffer and aborted.
#
# Needs bin/swc (make swc). Exit non-zero if any case fails.

set -u
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"

if [ ! -x "$SWC" ]; then
    echo "error: swc not found at $SWC — run 'make swc' first" >&2
    exit 2
fi

fail=0
check() {  # name, got, want
    if [ "$2" = "$3" ]; then
        echo "PASS $1"
    else
        echo "FAIL $1: got '$2', want '$3'"
        fail=1
    fi
}

PROG='module M
fun main() { print(1) }'

out=$(printf '%s\n' "$PROG" | "$SWC" run /dev/stdin 2>&1); rc=$?
check "run /dev/stdin (pipe)" "$out" "1"
check "run /dev/stdin rc" "$rc" "0"

out=$("$SWC" run <(printf '%s\n' "$PROG") 2>&1); rc=$?
check "run process substitution" "$out" "1"
check "run process substitution rc" "$rc" "0"

TEST='module T
fun test_one() { assert_eq(1, 1) }'

out=$("$SWC" test <(printf '%s\n' "$TEST") 2>&1); rc=$?
case "$out" in
    *"1 passed"*) check "test process substitution" "passed" "passed" ;;
    *) check "test process substitution" "$out" "1 passed" ;;
esac
check "test process substitution rc" "$rc" "0"

exit $fail
