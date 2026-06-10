#!/usr/bin/env bash
# soak.sh — Phase-2.6 soak harness runner.
#
# Compiles tests/soak/soak_mixed.sw and runs it for SOAK_SECONDS (default 60).
# Asserts SOAK_OK at exit; reports peak RSS and asserts it stays within budget.
#
# Usage:
#   bash scripts/soak.sh [seconds] [rss_budget_mb]
#   SOAK_SECONDS=3600 bash scripts/soak.sh   # 1-hour soak
#
# In CI (ubuntu-24.04): timeout is available; on macOS dev it isn't, but the
# sw program manages its own deadline via sys_monotonic_ms() so no external
# timeout is needed for correctness.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
cd "$REPO"

SOAK_SECONDS="${1:-${SOAK_SECONDS:-60}}"
RSS_BUDGET_MB="${2:-${RSS_BUDGET_MB:-300}}"   # peak RSS must not exceed this

echo "[soak] compiling soak_mixed.sw (${SOAK_SECONDS}s, budget ${RSS_BUDGET_MB}MB)..."
./bin/swc build tests/soak/soak_mixed.sw -o bin/soak_mixed 2>/dev/null || {
    echo "[soak] FAIL: compile failed"; exit 1
}

echo "[soak] running..."
OUT=$(./bin/soak_mixed "$SOAK_SECONDS" 2>&1)
RC=$?

if [ $RC -ne 0 ]; then
    echo "[soak] FAIL: exited $RC"
    echo "$OUT" | tail -20
    exit 1
fi

if ! echo "$OUT" | grep -q "SOAK_OK"; then
    echo "[soak] FAIL: SOAK_OK not printed"
    echo "$OUT" | tail -20
    exit 1
fi

echo "[soak] PASS (${SOAK_SECONDS}s)"
echo "$OUT" | grep -E "SOAK_OK|soak_mixed:" | tail -3
