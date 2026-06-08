#!/usr/bin/env bash
# gc_asan_run.sh <test.sw> [tag] — compile a COMPILED sw program twice (normal
# baseline + ASAN/-DSW_ARENA_POISON), run both, print a one-line VERDICT for the
# GC v1 copy-on-escape implementation.
#
#   PASS           both builds run clean
#   GC_BUG         baseline runs clean but the ASAN/poison build faults, reads
#                  0xDE garbage (a content assert FAILs), or exits non-zero
#   BASELINE_FAIL  doesn't compile/run even normally → test bug or non-GC bug
#
# CAVEAT (false positive): ASAN cannot track swarmrt's custom swapcontext/asm
# coroutine stacks (swarmrt_asm.S). In a PANIC-heavy test, a worker's no_return
# context swap leaves abandoned stack scopes poisoned; a later recycled worker
# can then trip a false *stack*-use-after-scope inside e.g. _builtin_to_string.
# That is NOT a GC bug — tell it apart by the backtrace: a real GC defect is a
# HEAP use-after-free (or a 0xDE-poison CONTENT mismatch) read in the PARENT
# after settle(); the false positive is a stack issue on a WORKER fiber. ASAN's
# heap-UAF + the 0xDE arena poison (the GC-relevant checks) are unaffected.
#
# Unique per-tag temp names so many invocations can run without clobbering.
set -u
# Repo root = parent of this script's dir (no hardcoded paths). Override with SWARMRT_REPO.
REPO="${SWARMRT_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)}"
cd "$REPO" || { echo "VERDICT: BASELINE_FAIL (no repo at $REPO)"; exit 99; }
SW="${1:?usage: gc_asan_run.sh <test.sw> [tag]}"; TAG="${2:-$$}"
b="/tmp/gctest_${TAG}"
# Portable timeout: gtimeout (coreutils) if present, else run with no limit.
if command -v gtimeout >/dev/null 2>&1; then TO() { gtimeout "$@"; }
elif command -v timeout >/dev/null 2>&1; then TO() { timeout "$@"; }
else TO() { shift; "$@"; }; fi
SQ=$(brew --prefix sqlite3 2>/dev/null); OS=$(brew --prefix openssl@3 2>/dev/null)
RT="src/swarmrt_native.c src/swarmrt_asm.S src/swarmrt_otp.c src/swarmrt_task.c src/swarmrt_ets.c src/swarmrt_phase4.c src/swarmrt_phase5.c src/swarmrt_hotload.c src/swarmrt_io.c src/swarmrt_gc.c src/swarmrt_node.c src/swarmrt_lang.c src/swarmrt_http.c src/swarmrt_pdf.c src/swarmrt_varena.c"

# 1. baseline build + run (normal swc build, GC live, no sanitizer)
if ! ./bin/swc build "$SW" -o "${b}_base" >/dev/null 2>"${b}_base.err"; then
  echo "VERDICT: BASELINE_FAIL (compile)"; grep -iE "error|cannot" "${b}_base.err" | grep -v writable | head -8; exit 1
fi
BASE_OUT=$(TO 120 "${b}_base" 2>&1); BASE_RC=$?
if [ $BASE_RC -ne 0 ]; then echo "VERDICT: BASELINE_FAIL (run rc=$BASE_RC)"; echo "$BASE_OUT" | grep -vE "SwarmRT\]" | tail -15; exit 1; fi

# 2. emit C, compile under ASAN + poison
./bin/swc build --emit-c "$SW" -o "${b}_emit" >/dev/null 2>&1 || true
GEN="${SW%.sw}.gen.c"
if [ ! -f "$GEN" ]; then echo "VERDICT: BASELINE_FAIL (no gen.c)"; exit 1; fi
cc -fsanitize=address -fno-sanitize-recover=all -DSW_ARENA_POISON -g -O1 -pthread \
   -D_DARWIN_C_SOURCE -Wno-macro-redefined -DSWARMRT_TLS -DSWARMRT_OPENSSL_PREFIX="\"$OS\"" \
   -Isrc -I"$SQ/include" -I"$OS/include" "$GEN" $RT \
   -L"$SQ/lib" -L"$OS/lib" -lz -lsqlite3 -lssl -lcrypto -lm -o "${b}_asan" 2>"${b}_asan.cc.err"
rm -f "$GEN"
if [ ! -x "${b}_asan" ]; then echo "VERDICT: BASELINE_FAIL (asan compile)"; grep -iE "error" "${b}_asan.cc.err" | grep -v writable | head -8; exit 1; fi

# 3. run under ASAN + poison
ASAN_OUT=$(ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 TO 240 "${b}_asan" 2>&1); ASAN_RC=$?
if echo "$ASAN_OUT" | grep -qiE "AddressSanitizer|use-after-free|heap-buffer-overflow|SEGV|runtime error|^FAIL |[^A-Za-z]FAIL "; then
  echo "VERDICT: GC_BUG"; echo "$ASAN_OUT" | grep -iE "AddressSanitizer|use-after-free|heap-buffer|SEGV|runtime error|FAIL |  #[0-9]" | head -30; exit 2
fi
if [ $ASAN_RC -ne 0 ]; then echo "VERDICT: GC_BUG (asan rc=$ASAN_RC)"; echo "$ASAN_OUT" | grep -vE "SwarmRT\]" | tail -25; exit 2; fi
echo "VERDICT: PASS"; echo "$ASAN_OUT" | grep -vE "SwarmRT\]" | tail -3
rm -f "${b}_base" "${b}_asan" "${b}"*.err
exit 0
