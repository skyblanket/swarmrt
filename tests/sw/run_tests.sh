#!/bin/bash
# Test driver for sw-language tests.
#
# Compiles each tests/sw/test_*.sw with bin/swc, runs the resulting
# binary, and reports pass/fail. Exit non-zero if any file fails.
#
# Each test file's main() is expected to:
#   - Print PASS / FAIL lines per assertion
#   - Print a summary line ("OK <name> N/N" or "FAIL <name> ...")
#   - sys_exit(0) on full pass, sys_exit(1) if any assertion failed
#
# Compile/run output of each file is captured. On failure we print the
# captured output for diagnosis; on success we print only the final
# summary line.

set -u
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"
TESTS_DIR="$SWARMRT_ROOT/tests/sw"
BUILD_DIR="$SWARMRT_ROOT/tests/sw/.build"

if [ ! -x "$SWC" ]; then
    echo "error: swc not found at $SWC — run 'make swc libswarmrt' first" >&2
    exit 2
fi

mkdir -p "$BUILD_DIR"

# Color helpers (skip if not a TTY).
if [ -t 1 ]; then
    GREEN=$'\e[32m' ; RED=$'\e[31m' ; DIM=$'\e[2m' ; RESET=$'\e[0m'
else
    GREEN='' ; RED='' ; DIM='' ; RESET=''
fi

total_files=0
failed_files=0
total_assertions=0
failed_assertions=0

for sw in "$TESTS_DIR"/test_*.sw; do
    [ -e "$sw" ] || continue
    name="$(basename "$sw" .sw)"
    bin="$BUILD_DIR/$name"
    log="$BUILD_DIR/$name.log"

    total_files=$((total_files + 1))

    # Compile
    if ! "$SWC" build "$sw" -o "$bin" >"$log" 2>&1; then
        failed_files=$((failed_files + 1))
        echo "${RED}COMPILE FAIL${RESET} $name"
        sed 's/^/    /' "$log"
        continue
    fi

    # Run
    if "$bin" >"$log" 2>&1; then
        # Pull the summary line + count any PASS lines for the rollup.
        passes=$(grep -c '^PASS ' "$log" || true)
        total_assertions=$((total_assertions + passes))
        summary=$(grep -E '^OK |^FAIL ' "$log" | tail -1)
        echo "${GREEN}OK${RESET}           $name ${DIM}— $summary${RESET}"
    else
        failed_files=$((failed_files + 1))
        passes=$(grep -c '^PASS ' "$log" || true)
        fails=$(grep -c '^FAIL ' "$log" || true)
        total_assertions=$((total_assertions + passes))
        failed_assertions=$((failed_assertions + fails))
        echo "${RED}RUN FAIL${RESET}     $name"
        sed 's/^/    /' "$log"
    fi
done

echo ""
if [ "$failed_files" -eq 0 ]; then
    echo "${GREEN}all sw tests passed${RESET} — $total_files files, $total_assertions assertions"
    exit 0
else
    echo "${RED}$failed_files of $total_files files failed${RESET} ($failed_assertions failed assertions, $total_assertions passed)"
    exit 1
fi
