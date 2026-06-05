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

# === Interpreter-side tests (REPL builtin coverage) ======================
# Tests under tests/sw/repl/ are run through `swc test` (tree-walking
# interpreter) rather than compile+execute. They guard against the
# REPL/codegen builtin drift that the May 2026 marathon closed.
INTERP_DIR="$SWARMRT_ROOT/tests/sw/repl"
interp_files=0
interp_failed=0
if [ -d "$INTERP_DIR" ]; then
    echo "--- interpreter (swc test) ---"
    for sw in "$INTERP_DIR"/test_*.sw; do
        [ -e "$sw" ] || continue
        interp_files=$((interp_files + 1))
        name="$(basename "$sw" .sw)"
        log="$BUILD_DIR/$name.interp.log"
        if "$SWC" test "$sw" >"$log" 2>&1; then
            summary=$(grep -E '^  [0-9]+ tests' "$log" | tail -1 | sed 's/^  //')
            echo "${GREEN}OK${RESET}           $name ${DIM}— $summary${RESET}"
        else
            interp_failed=$((interp_failed + 1))
            echo "${RED}INTERP FAIL${RESET}  $name"
            sed 's/^/    /' "$log"
        fi
    done
    echo ""
fi

# === `swc run` end-to-end tests ==========================================
# Tests under tests/sw/run/ are executed via `swc run <file>` — the
# documented interpreter run path (import resolution + module merge +
# main() in the tree-walking interpreter). A test passes if `swc run`
# exits 0 (the program self-checks and sys_exit(1)s on any failure).
RUN_DIR="$SWARMRT_ROOT/tests/sw/run"
run_files=0
run_failed=0
if [ -d "$RUN_DIR" ]; then
    echo "--- swc run (interpreter end-to-end) ---"
    for sw in "$RUN_DIR"/test_*.sw; do
        [ -e "$sw" ] || continue
        run_files=$((run_files + 1))
        name="$(basename "$sw" .sw)"
        log="$BUILD_DIR/$name.run.log"
        if "$SWC" run "$sw" >"$log" 2>&1; then
            echo "${GREEN}OK${RESET}           $name ${DIM}— $(tail -1 "$log")${RESET}"
        else
            run_failed=$((run_failed + 1))
            echo "${RED}RUN(swc) FAIL${RESET} $name"
            sed 's/^/    /' "$log"
        fi
    done
    echo ""
fi

# === Watchdog stderr-clean regressions ===================================
# Tests under tests/sw/watchdog/ assert on *stderr*, not exit code: a
# fixture must run to completion WITHOUT the deadlock watchdog printing a
# spurious "possible deadlock" WARNING. We run each with a small
# SW_DEADLOCK_MS so the watchdog ticks several times during the idle
# window, then grep stderr. A warning = FAIL.
WD_DIR="$SWARMRT_ROOT/tests/sw/watchdog"
wd_files=0
wd_failed=0
if [ -d "$WD_DIR" ]; then
    echo "--- watchdog (stderr must stay clean) ---"
    for sw in "$WD_DIR"/*.sw; do
        [ -e "$sw" ] || continue
        wd_files=$((wd_files + 1))
        name="$(basename "$sw" .sw)"
        bin="$BUILD_DIR/wd_$name"
        elog="$BUILD_DIR/wd_$name.err"
        if ! "$SWC" build "$sw" -o "$bin" >"$BUILD_DIR/wd_$name.build" 2>&1; then
            wd_failed=$((wd_failed + 1))
            echo "${RED}COMPILE FAIL${RESET} $name"
            sed 's/^/    /' "$BUILD_DIR/wd_$name.build"
            continue
        fi
        # Fast watchdog; SW_QUIET silences the startup banner so the only
        # stderr we could see is a warning. perl alarm bounds the run
        # (macOS has no `timeout`).
        SW_QUIET=1 SW_DEADLOCK_MS=250 perl -e 'alarm 6; exec @ARGV or die' \
            "$bin" >"$BUILD_DIR/wd_$name.out" 2>"$elog"
        if grep -q "possible deadlock" "$elog"; then
            wd_failed=$((wd_failed + 1))
            echo "${RED}WATCHDOG FAIL${RESET} $name — spurious deadlock warning:"
            sed 's/^/    /' "$elog"
        else
            echo "${GREEN}OK${RESET}           $name ${DIM}— no spurious deadlock warning${RESET}"
        fi
    done
    echo ""
fi

# === In-VM wsc_connect single-scheduler regressions ======================
# Tests under tests/sw/invm_wsc/ MUST run with SW_SCHEDULERS=1 — that's the
# configuration that pinned the in-VM wsc_connect deadlock (a blocking
# connect()/handshake recv() on the only scheduler thread starved the in-VM WS
# server that had to complete the handshake). The fix makes connect/handshake
# yield the scheduler. A regression re-introduces the hang, so we bound each run
# with a perl alarm (macOS has no `timeout`); a non-zero exit (incl. SIGALRM)
# is a FAIL.
INVM_DIR="$SWARMRT_ROOT/tests/sw/invm_wsc"
invm_files=0
invm_failed=0
if [ -d "$INVM_DIR" ]; then
    echo "--- in-VM wsc_connect (SW_SCHEDULERS=1, must not deadlock) ---"
    for sw in "$INVM_DIR"/test_*.sw; do
        [ -e "$sw" ] || continue
        invm_files=$((invm_files + 1))
        name="$(basename "$sw" .sw)"
        bin="$BUILD_DIR/invm_$name"
        log="$BUILD_DIR/invm_$name.log"
        if ! "$SWC" build "$sw" -o "$bin" >"$log" 2>&1; then
            invm_failed=$((invm_failed + 1))
            echo "${RED}COMPILE FAIL${RESET} $name"
            sed 's/^/    /' "$log"
            continue
        fi
        # Single scheduler + a 60s alarm: the happy path is ~instant, but a real
        # deadlock hangs forever — so a generous alarm still catches a regression
        # while tolerating CPU contention (e.g. right after a full rebuild, where
        # a 20s bound flaked even though the two-server in-VM WS dance was fine).
        # This fixture's pass/fail is load-sensitive: its in-test 25s idle guard
        # ("tx_handler idle") can trip under heavy CPU contention even though the
        # code is fine (verified: pristine main flakes at the same rate under
        # load). Retry up to 3x — a transient load spike shouldn't red the suite,
        # but a REAL reintroduced deadlock fails all 3 (each bounded by the 60s
        # alarm). Pass on the first attempt that exits 0.
        rc=1
        for _attempt in 1 2 3; do
            SW_QUIET=1 SW_SCHEDULERS=1 perl -e 'alarm 60; exec @ARGV or die' \
                "$bin" >"$log" 2>&1
            rc=$?
            [ "$rc" -eq 0 ] && break
        done
        if [ "$rc" -eq 0 ]; then
            summary=$(grep -E '^OK |^FAIL ' "$log" | tail -1)
            passes=$(grep -c '^PASS ' "$log" || true)
            total_assertions=$((total_assertions + passes))
            echo "${GREEN}OK${RESET}           $name ${DIM}— $summary${RESET}"
        else
            invm_failed=$((invm_failed + 1))
            if [ "$rc" -eq 142 ]; then
                echo "${RED}DEADLOCK${RESET}     $name — timed out (alarm); in-VM wsc_connect hung"
            else
                echo "${RED}RUN FAIL${RESET}     $name (exit $rc)"
            fi
            sed 's/^/    /' "$log"
        fi
    done
    echo ""
fi

total_all_files=$((total_files + interp_files + run_files + wd_files + invm_files))
total_all_failed=$((failed_files + interp_failed + run_failed + wd_failed + invm_failed))

if [ "$total_all_failed" -eq 0 ]; then
    echo "${GREEN}all sw tests passed${RESET} — $total_all_files files, $total_assertions assertions"
    exit 0
else
    echo "${RED}$total_all_failed of $total_all_files files failed${RESET} ($failed_assertions failed assertions, $total_assertions passed)"
    exit 1
fi
