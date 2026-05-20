#!/bin/bash
# compare_runs.sh — diff two eval runs and emit a side-by-side delta table.
#
# Usage:  ./compare_runs.sh <baseline_run_id> <new_run_id>
# Example: ./compare_runs.sh 20260520_233118 20260521_003437

set -u
cd "$(dirname "$0")"
RESULTS_DIR="results"
BASE="$1"
NEW="$2"

if [ ! -d "$RESULTS_DIR/$BASE" ] || [ ! -d "$RESULTS_DIR/$NEW" ]; then
    echo "missing run dir(s): $RESULTS_DIR/$BASE and/or $RESULTS_DIR/$NEW" >&2
    exit 1
fi

models=$(jq -r '.models[].id' models.json)
prompts=$(ls prompts/*.md | xargs -n1 basename | sed 's/\.md$//' | sort)

# Per-model summary
echo "## Pass rate by model — baseline vs after fixes"
echo ""
echo "| Model | Baseline | After fixes | Δ |"
echo "|---|---:|---:|---:|"
for m in $models; do
    # Count an attempt whenever the model dir exists for a prompt (even
    # if compile/run failed and no actual.txt was written).
    base_pass=0; base_total=0
    new_pass=0; new_total=0
    for p in $prompts; do
        for run in "$BASE" "$NEW"; do
            d="$RESULTS_DIR/$run/$p/$m"
            [ -d "$d" ] || continue
            if [ "$run" = "$BASE" ]; then base_total=$((base_total + 1)); else new_total=$((new_total + 1)); fi
            a="$d/actual.txt"
            e="$d/expected.txt"
            if [ -f "$a" ] && [ -f "$e" ] && [ "$(cat "$a")" = "$(cat "$e")" ]; then
                if [ "$run" = "$BASE" ]; then base_pass=$((base_pass + 1)); else new_pass=$((new_pass + 1)); fi
            fi
        done
    done
    if [ "$base_total" -gt 0 ]; then
        base_rate="${base_pass}/${base_total} ($(awk -v p="$base_pass" -v n="$base_total" 'BEGIN { printf "%.0f%%", (p/n)*100 }'))"
    else
        base_rate="—"
    fi
    if [ "$new_total" -gt 0 ]; then
        new_rate="${new_pass}/${new_total} ($(awk -v p="$new_pass" -v n="$new_total" 'BEGIN { printf "%.0f%%", (p/n)*100 }'))"
        delta=$((new_pass - base_pass))
        if [ "$delta" -gt 0 ]; then delta="+$delta"; fi
    else
        new_rate="—"
        delta="—"
    fi
    echo "| $m | $base_rate | $new_rate | $delta |"
done

echo ""
echo "## Per-prompt × per-model — baseline → after fixes"
echo ""
printf "| Prompt |"
for m in $models; do printf " %s |" "$m"; done
printf "\n|---|"
for m in $models; do printf ":---:|"; done
printf "\n"
for p in $prompts; do
    printf "| %s |" "$p"
    for m in $models; do
        b_a="$RESULTS_DIR/$BASE/$p/$m/actual.txt"
        b_e="$RESULTS_DIR/$BASE/$p/$m/expected.txt"
        n_a="$RESULTS_DIR/$NEW/$p/$m/actual.txt"
        n_e="$RESULTS_DIR/$NEW/$p/$m/expected.txt"
        if [ -f "$b_a" ] && [ -f "$b_e" ] && [ "$(cat "$b_a")" = "$(cat "$b_e")" ]; then b="✓"; else b="✗"; fi
        if [ -f "$n_a" ] && [ -f "$n_e" ] && [ "$(cat "$n_a")" = "$(cat "$n_e")" ]; then n="✓"; else n="✗"; fi
        printf " %s → %s |" "$b" "$n"
    done
    printf "\n"
done
