#!/usr/bin/env bash
# gc_trace_check.sh <test.sw> [tag] — gate for cross-process panic-TRACE isolation.
#
# Unlike the exit-code repros, panic call chains are observable only on stderr, so
# this builds the program under ASAN + -DSW_ARENA_POISON (uniform with gc-stress;
# the bug is diagnostic, not a memory fault — ASAN just confirms the trace path is
# clean), runs it under SW_SCHEDULERS=1, and inspects the panic banner:
#
#   PASS   the victim's panic chain contains "xvictim" and NOT "xcontam"
#   FAIL   stderr contains "xcontam" — a foreign process's frames leaked into the
#          victim's panic call chain (the cross-fiber trace-TLS bug), OR the
#          expected "xvictim" panic banner never appeared (test didn't run right)
#
# "xcontam" appears in stderr ONLY if a contaminant frame is printed, and the
# contaminant never panics — so its presence is unambiguous contamination.
set -u
REPO="${SWARMRT_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)}"
cd "$REPO" || { echo "VERDICT: FAIL (no repo at $REPO)"; exit 99; }
SW="${1:?usage: gc_trace_check.sh <test.sw> [tag]}"; TAG="${2:-$$}"
b="/tmp/gctrace_${TAG}"
if command -v gtimeout >/dev/null 2>&1; then TO() { gtimeout "$@"; }
elif command -v timeout >/dev/null 2>&1; then TO() { timeout "$@"; }
else TO() { shift; "$@"; }; fi
SQ=$(brew --prefix sqlite3 2>/dev/null); OS=$(brew --prefix openssl@3 2>/dev/null)
RT="src/swarmrt_native.c src/swarmrt_asm.S src/swarmrt_otp.c src/swarmrt_task.c src/swarmrt_ets.c src/swarmrt_phase4.c src/swarmrt_phase5.c src/swarmrt_hotload.c src/swarmrt_io.c src/swarmrt_gc.c src/swarmrt_node.c src/swarmrt_lang.c src/swarmrt_http.c src/swarmrt_pdf.c src/swarmrt_varena.c"

./bin/swc build --emit-c "$SW" -o "${b}_emit" >/dev/null 2>&1 || true
GEN="${SW%.sw}.gen.c"
if [ ! -f "$GEN" ]; then echo "VERDICT: FAIL (no gen.c)"; exit 1; fi
cc -fsanitize=address -fno-sanitize-recover=all -DSW_ARENA_POISON -g -O1 -pthread \
   -D_DARWIN_C_SOURCE -Wno-macro-redefined -DSWARMRT_TLS -DSWARMRT_OPENSSL_PREFIX="\"$OS\"" \
   -Isrc -I"$SQ/include" -I"$OS/include" "$GEN" $RT \
   -L"$SQ/lib" -L"$OS/lib" -lz -lsqlite3 -lssl -lcrypto -lm -o "${b}_asan" 2>"${b}.cc.err"
rm -f "$GEN" "${b}_emit"
if [ ! -x "${b}_asan" ]; then echo "VERDICT: FAIL (compile)"; grep -iE "error" "${b}.cc.err" | grep -v writable | head -8; exit 1; fi

OUT=$(SW_SCHEDULERS=1 ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 TO 60 "${b}_asan" 2>&1)
if echo "$OUT" | grep -qiE "AddressSanitizer|use-after-free|heap-buffer-overflow|SEGV"; then
  echo "VERDICT: FAIL (memory error in the trace path)"; echo "$OUT" | grep -iE "AddressSanitizer|use-after-free|SEGV|  #[0-9]" | head -20; exit 2
fi
if ! echo "$OUT" | grep -q "XVICTIM_PANIC"; then
  echo "VERDICT: FAIL (victim never panicked — interleave didn't hit)"; echo "$OUT" | grep -vE "SwarmRT\]" | tail -15; exit 1
fi
if echo "$OUT" | grep -q "xcontam"; then
  echo "VERDICT: FAIL (cross-process trace contamination — xcontam frames in the victim's panic chain)"
  echo "$OUT" | grep -A20 "XVICTIM_PANIC" | grep -E "xcontam|xvictim|call chain" | head -20
  exit 1
fi
echo "VERDICT: PASS (victim panic chain is xvictim-only; no foreign frames)"
echo "$OUT" | grep -E "xvictim|call chain" | head -6
rm -f "${b}_asan" "${b}.cc.err"
exit 0
