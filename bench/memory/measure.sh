#!/usr/bin/env bash
# bench/memory/measure.sh — build bench/memory/ring.sw and print one JSON line
# per (mode, N) with the per-process resident memory. Report-only; see
# docs/design/PROCESS_WEIGHT.md (Phase 0).
#
#   bash bench/memory/measure.sh               # busy + idle at N=10000 and 30000
#   NS="10000" MODES="busy" bash bench/memory/measure.sh
#
# N stays at or below 30000 until processes stop owning a stack mapping: past
# ~32,700 processes stock Linux runs out of mappings (vm.max_map_count 65530).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
[ -x "$ROOT/bin/swc" ] || { echo "error: make swc libswarmrt first" >&2; exit 2; }
"$ROOT/bin/swc" build "$HERE/ring.sw" -O -o "$WORK/ring" >"$WORK/build.log" 2>&1 || { cat "$WORK/build.log" >&2; exit 1; }
for mode in ${MODES:-busy idle}; do
    for n in ${NS:-10000 30000}; do
        python3 "$HERE/measure.py" "$WORK/ring" --n "$n" --mode "$mode" \
            --schedulers "${SW_SCHEDULERS:-4}" --at "${AT:-6,8,10}"
    done
done
