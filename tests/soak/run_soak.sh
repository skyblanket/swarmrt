#!/bin/bash
# run_soak.sh — Phase 2.6 mixed-workload soak.
#
# Runs tests/soak/soak.sw for SOAK_SECONDS (default 60) while sampling RSS,
# and asserts:
#   1. it exits 0 and prints PROBE_OK (no crash, no hang),
#   2. stderr stays clean (no deadlock-watchdog warning, no SIGSEGV banner),
#   3. peak RSS stays under SOAK_RSS_BUDGET_MB (default 256) — the whole
#      mixed workload holds flat, not just the per-owner slope probes.
#
# CI runs the 60s smoke. The full 24h soak is the SAME binary:
#   SOAK_SECONDS=86400 SOAK_RSS_BUDGET_MB=512 ./tests/soak/run_soak.sh
# on a dedicated host (documented in PRODUCTION_ROADMAP.md 2.6).

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$ROOT/bin/swc"
BIN="$ROOT/tests/soak/.build/soak"
SECS="${SOAK_SECONDS:-60}"
BUDGET="${SOAK_RSS_BUDGET_MB:-256}"

[ -x "$SWC" ] || { echo "error: build swc first"; exit 2; }
mkdir -p "$(dirname "$BIN")"
"$SWC" build "$ROOT/tests/soak/soak.sw" -o "$BIN" >/dev/null 2>&1 || { echo "soak: compile FAIL"; exit 1; }

echo "soak: running ${SECS}s, RSS budget ${BUDGET}MB"
SW_QUIET=1 "$BIN" "$SECS" >"$BIN.out" 2>"$BIN.err" &
pid=$!

# RSS sampler (KB), portable: /proc/<pid>/status VmRSS on Linux (cheap, no
# fork); `ps -o rss=` elsewhere (macOS has no /proc, so the old /proc-only
# reader silently sampled 0 → the RSS budget check passed without measuring
# anything). ps reports RSS in KB on both macOS and Linux.
if [ -d /proc ]; then rss_of() { awk '/VmRSS/{print $2}' "/proc/$1/status" 2>/dev/null; }
else                  rss_of() { ps -o rss= -p "$1" 2>/dev/null | tr -d ' '; }
fi
peak_kb=0
while kill -0 "$pid" 2>/dev/null; do
    rss=$(rss_of "$pid"); [ -z "$rss" ] && rss=0
    [ "$rss" -gt "$peak_kb" ] 2>/dev/null && peak_kb=$rss
    sleep 1
done
wait "$pid"; ec=$?
peak_mb=$(( peak_kb / 1024 ))

rounds=$(grep -oE "rounds=[0-9]+" "$BIN.out" | head -1)
fail=0
[ "$ec" -ne 0 ] && { echo "soak: FAIL (exit $ec)"; fail=1; }
grep -q "PROBE_OK" "$BIN.out" || { echo "soak: FAIL (no PROBE_OK — crashed/hung)"; fail=1; }
if grep -qiE "deadlock|CRASH|SIGSEGV|SIGABRT|AddressSanitizer" "$BIN.err"; then
    echo "soak: FAIL (stderr not clean):"; head -5 "$BIN.err"; fail=1
fi
if [ "$peak_mb" -gt "$BUDGET" ]; then
    echo "soak: FAIL (peak RSS ${peak_mb}MB > ${BUDGET}MB budget)"; fail=1
fi

[ "$fail" -eq 0 ] && echo "soak: PASS (${SECS}s, $rounds, peak RSS ${peak_mb}MB <= ${BUDGET}MB, clean exit)"
exit $fail
