#!/bin/bash
# Dual-path conformance gate.
#
# The language's core promise is that a program means ONE thing — yet it
# has two execution paths (the tree-walking interpreter behind
# `swc run`/REPL/`swc test`, and the AOT C codegen behind `swc build`).
# Every divergence found so far (case-guard UB, try/catch-vs-panic,
# receive default timeout) was exactly this class of bug.
#
# This gate runs every tests/sw/conform/*.sw through BOTH paths and
# fails on ANY difference in stdout or exit code. Conform programs are
# pure-functional by design (no spawn/receive/timers — the interpreter
# does not run the scheduler), deterministic, and print their results.
#
# A program that panics is fine — both paths must then panic with the
# same stdout prefix and the same exit code. stderr is NOT compared
# (panic banners may legitimately differ in trace detail), but stdout
# and exit codes must be byte-identical.

set -u
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"
CONFORM_DIR="$SWARMRT_ROOT/tests/sw/conform"
BUILD_DIR="$SWARMRT_ROOT/tests/sw/.build/conform"

if [ ! -x "$SWC" ]; then
    echo "error: swc not found at $SWC — run 'make swc libswarmrt' first" >&2
    exit 2
fi

mkdir -p "$BUILD_DIR"

if [ -t 1 ]; then
    GREEN=$'\e[32m' ; RED=$'\e[31m' ; DIM=$'\e[2m' ; RESET=$'\e[0m'
else
    GREEN='' ; RED='' ; DIM='' ; RESET=''
fi

total=0
failed=0

for sw in "$CONFORM_DIR"/t*.sw; do
    [ -e "$sw" ] || continue
    name="$(basename "$sw" .sw)"
    bin="$BUILD_DIR/$name"
    total=$((total + 1))

    # Path 1: interpreter
    SW_QUIET=1 "$SWC" run "$sw" > "$BUILD_DIR/$name.interp.out" 2>"$BUILD_DIR/$name.interp.err"
    interp_rc=$?

    # Path 2: compiled
    if ! SW_QUIET=1 "$SWC" build "$sw" -o "$bin" > "$BUILD_DIR/$name.cc.log" 2>&1; then
        failed=$((failed + 1))
        echo "${RED}COMPILE FAIL${RESET} $name"
        sed 's/^/    /' "$BUILD_DIR/$name.cc.log" | head -15
        continue
    fi
    SW_QUIET=1 "$bin" > "$BUILD_DIR/$name.comp.out" 2>"$BUILD_DIR/$name.comp.err"
    comp_rc=$?

    # Compare stdout + exit code
    if ! diff -u "$BUILD_DIR/$name.interp.out" "$BUILD_DIR/$name.comp.out" \
            > "$BUILD_DIR/$name.diff" 2>&1; then
        failed=$((failed + 1))
        echo "${RED}DIVERGE${RESET}  $name — stdout differs (interpreter vs compiled):"
        sed 's/^/    /' "$BUILD_DIR/$name.diff" | head -25
        continue
    fi
    if [ "$interp_rc" -ne "$comp_rc" ]; then
        failed=$((failed + 1))
        echo "${RED}DIVERGE${RESET}  $name — exit codes differ: interpreter=$interp_rc compiled=$comp_rc"
        continue
    fi

    echo "${GREEN}OK${RESET}        $name ${DIM}(exit $comp_rc, $(wc -l < "$BUILD_DIR/$name.comp.out") lines)${RESET}"
done

echo
if [ "$failed" -eq 0 ]; then
    echo "conformance: all $total programs agree across interpreter + compiled"
    exit 0
else
    echo "conformance: $failed/$total programs DIVERGE between interpreter and compiled"
    exit 1
fi
