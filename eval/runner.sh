#!/bin/bash
# eval/runner.sh — drive each prompt × each model, score pass/fail.
#
# Usage:
#   ./runner.sh                # all prompts, all models in models.json
#   ./runner.sh prompts/03_*   # subset of prompts
#
# Required: bin/swc built, OTONOMY_KEY env var (and optionally
# GEMMA_KEY for the gemma endpoint).

set -u
cd "$(dirname "$0")"

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
COUNT_DIR="$RUN_DIR/.counts"
mkdir -p "$COUNT_DIR"

while IFS='|' read -r mid endpoint key_env model_field label; do
    [ -z "$mid" ] && continue
    echo "0" > "$COUNT_DIR/${mid}.pass"
    echo "0" > "$COUNT_DIR/${mid}.fail"
    echo "$label" > "$COUNT_DIR/${mid}.label"
done <<< "$MODELS"

bump() {
    local mid="$1"
    local outcome="$2"  # pass or fail
    local f="$COUNT_DIR/${mid}.${outcome}"
    local cur=$(cat "$f")
    echo $((cur + 1)) > "$f"
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

        local t0=$(date +%s)
        local response
        response=$(query_model "$endpoint" "$key_val" "$model_field" "$prompt_body" 2>"$model_dir/curl.err")
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
            echo "$response" > "$model_dir/response_raw.json"
            echo "  ${RED}FAIL${RESET}  ${mid} (no content from API, see response_raw.json)"
            bump "$mid" fail
            continue
        fi
        echo "$content" > "$model_dir/response.txt"

        local code
        code=$(extract_sw_block "$content")
        echo "$code" > "$model_dir/program.sw"

        # Compile
        if ! "$SWC" build "$model_dir/program.sw" -o "$model_dir/program" \
                >"$model_dir/compile.log" 2>&1; then
            echo "  ${RED}FAIL${RESET}  ${mid} ${DIM}(compile error, ${elapsed}s)${RESET}"
            bump "$mid" fail
            continue
        fi

        # Run. macOS doesn't ship `timeout(1)`; if it's installed via
        # coreutils we use it, otherwise we just trust the program to
        # finish (compiled sw is usually < 1s; pathological loops will
        # be killed when the shell session exits).
        local actual
        local timeout_cmd=""
        if command -v gtimeout >/dev/null 2>&1; then timeout_cmd="gtimeout 15"
        elif command -v timeout >/dev/null 2>&1; then timeout_cmd="timeout 15"
        fi
        actual=$($timeout_cmd "$model_dir/program" 2>"$model_dir/run.err" | normalize_output)
        echo "$actual" > "$model_dir/actual.txt"
        echo "$expected" > "$model_dir/expected.txt"

        if [ "$actual" = "$expected" ]; then
            echo "  ${GREEN}PASS${RESET}  ${mid} ${DIM}(${elapsed}s)${RESET}"
            bump "$mid" pass
        else
            echo "  ${RED}FAIL${RESET}  ${mid} ${DIM}(output mismatch, ${elapsed}s)${RESET}"
            diff <(echo "$expected") <(echo "$actual") > "$model_dir/diff.txt"
            bump "$mid" fail
        fi
    done <<< "$MODELS"
}

echo "${GREEN}== sw eval — $(date +'%Y-%m-%d %H:%M') ==${RESET}"
echo "run id: $RUN_ID"
echo "prompts: ${#PROMPTS[@]}"
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
    echo "| Model | Pass | Fail | Rate |"
    echo "|---|---:|---:|---:|"
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
    echo "  ${label}: ${GREEN}${p}${RESET}/${n} ${DIM}(${rate})${RESET}"
    echo "| ${label} | ${p} | ${f} | ${rate} |" >> "$SUMMARY_MD"
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
            actual_f="$RUN_DIR/$pid/$mid/actual.txt"
            expected_f="$RUN_DIR/$pid/$mid/expected.txt"
            if [ -f "$actual_f" ] && [ -f "$expected_f" ] && \
               [ "$(cat "$actual_f")" = "$(cat "$expected_f")" ]; then
                printf " ✓ |"
            else
                printf " ✗ |"
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
