#!/bin/bash
# eval/runner.sh — drive each prompt × each model, score pass/fail.
#
# Usage:
#   ./runner.sh                # all prompts, all models in models.json
#   ./runner.sh prompts/03_*   # subset of prompts
#
# Required: bin/swc built, OTONOMY_KEY env var (and optionally
# GEMMA_KEY for the gemma endpoint).
#
# k-sampling (honest statistics):
#   Each (prompt × model) cell is sampled K times (default EVAL_K=5).
#   We report pass@1 as mean ± stdev across the K samples per cell, plus
#   the aggregate pass rate over all K·prompts samples per model. temp=1
#   with a single draw is the worst possible statistic; K>1 with mean±stdev
#   is the cheapest credibility win.
#
#     EVAL_K=5 ./runner.sh        # default: 5 draws per cell
#     EVAL_K=1 ./runner.sh        # exactly reproduces the pre-k-sampling
#                                 # behavior (single draw, no stdev column)
#
# Backward compatibility: each cell still writes a representative
# actual.txt / expected.txt (from the first sample, k1) at the cell root,
# so compare_runs.sh and any existing tooling keep working unchanged. The
# per-sample artifacts live in <prompt>/<model>/kN/ subdirectories.

set -u
cd "$(dirname "$0")"

# Number of samples per (prompt × model) cell. Default 5; EVAL_K=1 gives
# the legacy single-shot behavior. Guard against non-numeric / <1 values.
EVAL_K="${EVAL_K:-5}"
case "$EVAL_K" in
    ''|*[!0-9]*) echo "error: EVAL_K must be a positive integer (got '$EVAL_K')" >&2; exit 2 ;;
esac
[ "$EVAL_K" -lt 1 ] && EVAL_K=1

EVAL_ROOT="$(pwd)"
SWARMRT_ROOT="$(cd .. && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"
SYSTEM_PROMPT="$(cat system_prompt.md)"

if [ ! -x "$SWC" ]; then
    echo "error: swc not at $SWC — run 'make swc libswarmrt' first" >&2
    exit 2
fi

# Color codes
if [ -t 1 ]; then
    GREEN=$'\e[32m' ; RED=$'\e[31m' ; YEL=$'\e[33m' ; DIM=$'\e[2m' ; RESET=$'\e[0m'
else
    GREEN='' ; RED='' ; YEL='' ; DIM='' ; RESET=''
fi

RUN_ID="$(date +%Y%m%d_%H%M%S)"
RESULTS_DIR="$EVAL_ROOT/results"
RUN_DIR="$RESULTS_DIR/$RUN_ID"
mkdir -p "$RUN_DIR"

# Models (parsed from models.json — each line: id|endpoint|key_env|model_field)
MODELS=$(jq -r '.models[] | "\(.id)|\(.endpoint)|\(.key_env)|\(.model_field)|\(.label)"' models.json)

# Prompts: default to all prompts/*.md, or args
if [ $# -gt 0 ]; then
    PROMPTS=("$@")
else
    PROMPTS=(prompts/*.md)
fi

# Aggregate counters per model (using files because macOS bash 3.2 has
# no associative arrays — feel free to upgrade to bash 4+ if you prefer
# associative arrays).
#
#   <mid>.pass / <mid>.fail   running totals over ALL samples (K·prompts)
#   <mid>.cells               one line per cell: "<passes_in_cell>/<K>"
#                             — used to compute mean ± stdev of per-cell
#                             pass@1 across prompts.
COUNT_DIR="$RUN_DIR/.counts"
mkdir -p "$COUNT_DIR"

while IFS='|' read -r mid endpoint key_env model_field label; do
    [ -z "$mid" ] && continue
    echo "0" > "$COUNT_DIR/${mid}.pass"
    echo "0" > "$COUNT_DIR/${mid}.fail"
    echo "$label" > "$COUNT_DIR/${mid}.label"
    : > "$COUNT_DIR/${mid}.cells"
done <<< "$MODELS"

bump() {
    local mid="$1"
    local outcome="$2"  # pass or fail
    local f="$COUNT_DIR/${mid}.${outcome}"
    local cur=$(cat "$f")
    echo $((cur + 1)) > "$f"
}

# Record the per-cell pass@1 numerator (how many of the cell's K samples
# passed) so the summary can compute mean ± stdev of pass@1 across cells.
record_cell() {
    local mid="$1"
    local cell_pass="$2"   # number of passing samples in this cell
    echo "${cell_pass}/${EVAL_K}" >> "$COUNT_DIR/${mid}.cells"
}

extract_sw_block() {
    # Pull the contents of the first ```sw ... ``` fence; if absent,
    # return the raw input (it might be naked code).
    local body="$1"
    local extracted
    extracted=$(echo "$body" | awk '
        /^```sw/ { in_block=1; next }
        in_block && /^```/ { exit }
        in_block { print }
    ')
    if [ -n "$extracted" ]; then
        echo "$extracted"
    else
        # Try ```sw with anything-after (e.g. ```sw\n)
        extracted=$(echo "$body" | awk '
            /^```/ && !done { fence_open = !fence_open; next }
            fence_open { print }
        ')
        if [ -n "$extracted" ]; then
            echo "$extracted"
        else
            echo "$body"
        fi
    fi
}

query_model() {
    local endpoint="$1"
    local key="$2"
    local model_field="$3"
    local prompt_body="$4"

    jq -n \
        --arg model "$model_field" \
        --arg sys "$SYSTEM_PROMPT" \
        --arg user "$prompt_body" \
        '{
            model: $model,
            messages: [
                {role: "system", content: $sys},
                {role: "user", content: $user}
            ],
            temperature: 1,
            max_tokens: 16384
        }' | curl -sS -m 120 -X POST "$endpoint" \
            -H "Content-Type: application/json" \
            -H "Authorization: Bearer $key" \
            -d @-
}

normalize_output() {
    # Strip the SwarmRT init banner, trim trailing whitespace.
    sed -E '/^\[SwarmRT\]/d' | sed -E 's/[[:space:]]+$//'
}

extract_expected() {
    local prompt_file="$1"
    awk '
        /^## Expected output/ { in_exp = 1; next }
        in_exp && /^```/ { in_block = !in_block; next }
        in_exp && in_block { print }
        in_exp && /^## / && !/^## Expected output/ { exit }
    ' "$prompt_file"
}

run_one() {
    local prompt_file="$1"
    local prompt_id="$(basename "$prompt_file" .md)"
    local prompt_body="$(cat "$prompt_file")"
    local expected="$(extract_expected "$prompt_file" | sed -E 's/[[:space:]]+$//')"

    echo ""
    echo "${DIM}── ${prompt_id} ──${RESET}"

    while IFS='|' read -r mid endpoint key_env model_field label; do
        [ -z "$mid" ] && continue
        local key_val="${!key_env:-}"
        if [ -z "$key_val" ]; then
            echo "  ${YEL}SKIP${RESET}  ${mid} (\$${key_env} unset)"
            continue
        fi

        local model_dir="$RUN_DIR/$prompt_id/$mid"
        mkdir -p "$model_dir"

        # Draw K independent samples for this cell. Each sample lands in
        # its own kN/ subdir; the first sample (k1) is also mirrored to
        # the cell root so compare_runs.sh / the summary matrix (which read
        # <cell>/actual.txt + expected.txt) keep working unchanged.
        local cell_pass=0
        local k
        for k in $(seq 1 "$EVAL_K"); do
            run_sample "$prompt_id" "$mid" "$endpoint" "$key_val" \
                       "$model_field" "$prompt_body" "$expected" "$model_dir" "$k"
            # run_sample returns 0 on a passing sample, nonzero otherwise.
            if [ $? -eq 0 ]; then
                cell_pass=$((cell_pass + 1))
            fi
        done

        record_cell "$mid" "$cell_pass"

        # Cell-level pass@1 line: "k/K passed (= rate)".
        local cell_rate
        cell_rate=$(awk -v p="$cell_pass" -v n="$EVAL_K" 'BEGIN { printf "%.0f%%", (p/n)*100 }')
        if [ "$cell_pass" -eq "$EVAL_K" ]; then
            echo "  ${GREEN}pass@1 ${cell_pass}/${EVAL_K}${RESET} ${DIM}(${cell_rate})${RESET}  ${mid}"
        elif [ "$cell_pass" -eq 0 ]; then
            echo "  ${RED}pass@1 ${cell_pass}/${EVAL_K}${RESET} ${DIM}(${cell_rate})${RESET}  ${mid}"
        else
            echo "  ${YEL}pass@1 ${cell_pass}/${EVAL_K}${RESET} ${DIM}(${cell_rate})${RESET}  ${mid}"
        fi
    done <<< "$MODELS"
}

# run_sample — draw ONE sample for a (prompt × model) cell, score it.
# Writes artifacts to <model_dir>/k<N>/ and, for k=1, mirrors actual.txt /
# expected.txt to <model_dir> for backward compatibility. Returns 0 iff
# this sample passed (compiled + ran + output matched).
run_sample() {
    local prompt_id="$1"
    local mid="$2"
    local endpoint="$3"
    local key_val="$4"
    local model_field="$5"
    local prompt_body="$6"
    local expected="$7"
    local model_dir="$8"
    local k="$9"

    local sdir="$model_dir/k${k}"
    mkdir -p "$sdir"

    # Tag for log lines: hide the kN suffix when K=1 so legacy output is
    # byte-for-byte familiar.
    local tag="$mid"
    [ "$EVAL_K" -gt 1 ] && tag="${mid} ${DIM}k${k}${RESET}"

    local t0=$(date +%s)
    local response
    response=$(query_model "$endpoint" "$key_val" "$model_field" "$prompt_body" 2>"$sdir/curl.err")
    local t1=$(date +%s)
    local elapsed=$((t1 - t0))

    # Extract content. Kimi reasoning models leave the answer in
    # `message.content`, but the chain-of-thought ends up in
    # `message.reasoning_content` — for our purposes we want the
    # answer, but if content is empty we fall back to reasoning.
    local content
    content=$(echo "$response" | jq -r '.choices[0].message.content // empty' 2>/dev/null)
    if [ -z "$content" ]; then
        content=$(echo "$response" | jq -r '.choices[0].message.reasoning_content // empty' 2>/dev/null)
    fi
    if [ -z "$content" ]; then
        # Anthropic shape fallback
        content=$(echo "$response" | jq -r '.content[0].text // empty' 2>/dev/null)
    fi
    if [ -z "$content" ]; then
        echo "$response" > "$sdir/response_raw.json"
        echo "    ${RED}FAIL${RESET}  ${tag} (no content from API, see k${k}/response_raw.json)"
        bump "$mid" fail
        [ "$k" -eq 1 ] && mirror_cell "$sdir" "$model_dir" "$expected"
        return 1
    fi
    echo "$content" > "$sdir/response.txt"

    local code
    code=$(extract_sw_block "$content")
    echo "$code" > "$sdir/program.sw"

    # Compile
    if ! "$SWC" build "$sdir/program.sw" -o "$sdir/program" \
            >"$sdir/compile.log" 2>&1; then
        echo "    ${RED}FAIL${RESET}  ${tag} ${DIM}(compile error, ${elapsed}s)${RESET}"
        bump "$mid" fail
        [ "$k" -eq 1 ] && mirror_cell "$sdir" "$model_dir" "$expected"
        return 1
    fi

    # Run with a hard timeout. macOS doesn't ship `timeout(1)`;
    # if `gtimeout` (coreutils) is installed we use it, otherwise
    # fall back to a portable launch+sleep+kill pattern so a hung
    # LLM-generated program (e.g. infinite receive) doesn't stall
    # the whole eval. Without this, a single bad program can pin
    # the runner indefinitely.
    local actual
    if command -v gtimeout >/dev/null 2>&1; then
        actual=$(gtimeout 15 "$sdir/program" 2>"$sdir/run.err" | normalize_output)
    elif command -v timeout >/dev/null 2>&1; then
        actual=$(timeout 15 "$sdir/program" 2>"$sdir/run.err" | normalize_output)
    else
        # Portable timeout: launch in background, sleep, kill.
        # Wrap the watchdog in a subshell with stderr to /dev/null so
        # the "Terminated: 15" job-control message doesn't leak into
        # the eval output when the program finishes before the killer.
        "$sdir/program" > "$sdir/run.out" 2>"$sdir/run.err" &
        local pid=$!
        ( sleep 15 && kill -9 "$pid" 2>/dev/null ) 2>/dev/null &
        local killer=$!
        wait "$pid" 2>/dev/null
        kill "$killer" 2>/dev/null
        wait "$killer" 2>/dev/null
        actual=$(cat "$sdir/run.out" | normalize_output)
        rm -f "$sdir/run.out"
    fi
    echo "$actual" > "$sdir/actual.txt"
    echo "$expected" > "$sdir/expected.txt"
    [ "$k" -eq 1 ] && mirror_cell "$sdir" "$model_dir" "$expected"

    if [ "$actual" = "$expected" ]; then
        echo "    ${GREEN}PASS${RESET}  ${tag} ${DIM}(${elapsed}s)${RESET}"
        bump "$mid" pass
        return 0
    else
        echo "    ${RED}FAIL${RESET}  ${tag} ${DIM}(output mismatch, ${elapsed}s)${RESET}"
        diff <(echo "$expected") <(echo "$actual") > "$sdir/diff.txt"
        bump "$mid" fail
        return 1
    fi
}

# mirror_cell — copy a sample's actual.txt/expected.txt up to the cell
# root so compare_runs.sh and the summary matrix (which read the cell-root
# files) keep working exactly as before k-sampling existed.
mirror_cell() {
    local sdir="$1"
    local model_dir="$2"
    local expected="$3"
    if [ -f "$sdir/actual.txt" ]; then
        cp "$sdir/actual.txt" "$model_dir/actual.txt"
    else
        # No actual.txt was written (API/compile failure). Leave a
        # non-matching placeholder so the cell scores as a fail, matching
        # legacy behavior where such cells had no actual.txt at all.
        : > "$model_dir/actual.txt"
    fi
    echo "$expected" > "$model_dir/expected.txt"
}

# cell_stats — given a model's .cells file (one "<pass>/<K>" line per cell),
# print "mean ± stdev" of the per-cell pass@1 fraction across cells, as
# percentages. With a single cell, stdev is 0. With no cells, prints "—".
# Population stdev (divide by N) — we are describing this run's cells, not
# inferring a wider population.
cell_stats() {
    local cells_file="$1"
    [ -s "$cells_file" ] || { echo "—"; return; }
    awk -F/ '
        { p=$1; k=$2; if (k>0) { frac=p/k; sum+=frac; sumsq+=frac*frac; n++ } }
        END {
            if (n==0) { print "—"; exit }
            mean = sum/n
            var  = (sumsq/n) - (mean*mean)
            if (var < 0) var = 0          # guard fp noise
            sd = sqrt(var)
            printf "%.0f%% ± %.0f%%", mean*100, sd*100
        }' "$cells_file"
}

echo "${GREEN}== sw eval — $(date +'%Y-%m-%d %H:%M') ==${RESET}"
echo "run id: $RUN_ID"
echo "prompts: ${#PROMPTS[@]}"
echo "samples per cell (EVAL_K): $EVAL_K"
echo ""

for p in "${PROMPTS[@]}"; do
    run_one "$p"
done

# Final summary
echo ""
echo "${GREEN}== summary ==${RESET}"

SUMMARY_MD="$RUN_DIR/summary.md"
{
    echo "# sw eval — run $RUN_ID"
    echo ""
    echo "_$(date +'%Y-%m-%d %H:%M:%S %Z')_"
    echo ""
    echo "## Pass rate by model"
    echo ""
    echo "_Each (prompt × model) cell sampled K=${EVAL_K} time(s) at temperature 1._"
    echo "_**pass@1** is the mean ± population-stdev of the per-cell pass@1 fraction across prompts;_"
    echo "_**Rate** is the raw aggregate over all ${EVAL_K}·prompts samples._"
    echo ""
    echo "| Model | Samples | Pass | Fail | Rate | pass@1 (mean ± stdev) |"
    echo "|---|---:|---:|---:|---:|:---:|"
} > "$SUMMARY_MD"

while IFS='|' read -r mid endpoint key_env model_field label; do
    [ -z "$mid" ] && continue
    p=$(cat "$COUNT_DIR/${mid}.pass" 2>/dev/null || echo 0)
    f=$(cat "$COUNT_DIR/${mid}.fail" 2>/dev/null || echo 0)
    n=$((p + f))
    if [ "$n" -eq 0 ]; then
        rate="—"
    else
        rate="$(awk -v p="$p" -v n="$n" 'BEGIN { printf "%.0f%%", (p / n) * 100 }')"
    fi
    pass1="$(cell_stats "$COUNT_DIR/${mid}.cells")"
    echo "  ${label}: ${GREEN}${p}${RESET}/${n} ${DIM}(${rate}, pass@1 ${pass1})${RESET}"
    echo "| ${label} | ${n} | ${p} | ${f} | ${rate} | ${pass1} |" >> "$SUMMARY_MD"
done <<< "$MODELS"

# Per-prompt × per-model matrix
{
    echo ""
    echo "## Per-prompt × per-model"
    echo ""
    printf "| Prompt |"
    while IFS='|' read -r mid endpoint key_env model_field label; do
        [ -z "$mid" ] && continue
        printf " %s |" "$mid"
    done <<< "$MODELS"
    printf "\n|---|"
    while IFS='|' read -r mid endpoint key_env model_field label; do
        [ -z "$mid" ] && continue
        printf ":---:|"
    done <<< "$MODELS"
    printf "\n"
    for p in "${PROMPTS[@]}"; do
        pid=$(basename "$p" .md)
        printf "| %s |" "$pid"
        while IFS='|' read -r mid endpoint key_env model_field label; do
            [ -z "$mid" ] && continue
            cell="$RUN_DIR/$pid/$mid"
            if [ "$EVAL_K" -gt 1 ]; then
                # Count passing samples across this cell's kN/ subdirs and
                # show the per-cell pass@1 fraction k/K.
                cpass=0
                for sa in "$cell"/k*/actual.txt; do
                    [ -f "$sa" ] || continue
                    se="$(dirname "$sa")/expected.txt"
                    [ -f "$se" ] || continue
                    if [ "$(cat "$sa")" = "$(cat "$se")" ]; then
                        cpass=$((cpass + 1))
                    fi
                done
                printf " %s/%s |" "$cpass" "$EVAL_K"
            else
                # Legacy single-shot rendering: ✓ / ✗ from the cell-root files.
                actual_f="$cell/actual.txt"
                expected_f="$cell/expected.txt"
                if [ -f "$actual_f" ] && [ -f "$expected_f" ] && \
                   [ "$(cat "$actual_f")" = "$(cat "$expected_f")" ]; then
                    printf " ✓ |"
                else
                    printf " ✗ |"
                fi
            fi
        done <<< "$MODELS"
        printf "\n"
    done
    echo ""
    echo "_See \`results/$RUN_ID/<prompt>/<model>/\` for each attempt:_"
    echo "_\`program.sw\` (generated code), \`compile.log\`, \`actual.txt\`, \`expected.txt\`, \`diff.txt\` (if mismatch)._"
} >> "$SUMMARY_MD"

echo ""
echo "${DIM}Results: $RUN_DIR${RESET}"
echo "${DIM}Summary: $SUMMARY_MD${RESET}"

# Mirror into results.md for stable link
cp "$SUMMARY_MD" "$RESULTS_DIR/results.md"
