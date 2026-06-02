#!/usr/bin/env bash
#
# check_sw_docs.sh — doc/example drift tripwire.
#
# COMPILE-CHECKS every complete sw program that ships in our docs and
# examples, so a copy-paste-able snippet can never silently rot. Two
# sources are checked:
#
#   1. Every fenced ```sw block in the doc set below that is a COMPLETE
#      PROGRAM (contains both a `module` declaration and a `fun main(`).
#      Illustrative fragments (a lone function, a `receive` arm, a
#      `try/catch` snippet, etc.) are intentionally skipped — they are
#      not whole programs and `swc build` would (correctly) reject them.
#
#   2. Every examples/*.sw that has a `fun main(` (i.e. a runnable
#      program). Library-only files (mathlib.sw) and test files
#      (*_test.sw — `swc test` fodder, no main) are skipped.
#
# Each program is COMPILE-CHECKED only: `swc build` runs the full
# parse → codegen → cc pipeline, and we throw the binary away. We never
# RUN it — many of these need a network, an LLM endpoint, or live actors
# to do anything, and a green *compile* is all this tripwire promises.
#
# A single parse / codegen / cc failure fails the whole run (exit 1) and
# prints the offending block (file + source line) plus swc's own error.
#
# Run from the repo root:  bash scripts/check_sw_docs.sh
# Needs:  ./bin/swc  (build it first with `make swc libswarmrt`).

set -u

# --- locate repo root (parent of this script's dir) so the checker works
# --- regardless of the caller's cwd. swc resolves the stdlib (Std, LLM,
# --- Mcp, ...) relative to its own location (<swc_dir>/../lib), so we
# --- must invoke ./bin/swc from the repo root for those imports to find
# --- lib/.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

SWC="./bin/swc"
if [ ! -x "$SWC" ]; then
    echo "check_sw_docs: $SWC not found or not executable." >&2
    echo "check_sw_docs: build it first with: make swc libswarmrt" >&2
    exit 2
fi

# Docs whose ```sw blocks we hold to a compile bar.
DOC_FILES=(
    "docs/SW_LANGUAGE.md"
    "docs/AGENT_SYSTEM.md"
    "docs/BUILDING_AGENTS.md"
    "eval/system_prompt.md"
)

WORK="$(mktemp -d "${TMPDIR:-/tmp}/sw_doc_check.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

FAILURES=0
CHECKED=0

# Compile-check a single .sw file. $1 = path to .sw, $2 = human label.
# On failure: bump FAILURES and print swc's error. Binary is discarded.
compile_check() {
    local src="$1" label="$2" out log
    out="$WORK/out.bin"
    log="$WORK/swc.log"
    if "$SWC" build "$src" -o "$out" >"$log" 2>&1; then
        CHECKED=$((CHECKED + 1))
        echo "  ok    $label"
    else
        FAILURES=$((FAILURES + 1))
        echo "  FAIL  $label" >&2
        echo "  ----- swc output -------------------------------------------" >&2
        sed 's/^/  | /' "$log" >&2
        echo "  ----- offending program ------------------------------------" >&2
        sed 's/^/  > /' "$src" >&2
        echo "  ------------------------------------------------------------" >&2
    fi
    rm -f "$out"
}

# --- awk extractor: print each COMPLETE-PROGRAM ```sw block as
# ---   @@BLOCK <startline>\n<de-indented body>\n@@END
# --- A block qualifies only if it has both a `module` line and a
# --- `fun main(` line. Leading indentation on the opening fence (e.g.
# --- list-nested ```sw blocks) is stripped from every body line.
read -r -d '' EXTRACT_AWK <<'AWK'
BEGIN { in_block = 0 }
{
    line = $0
    if (in_block == 0) {
        if (match(line, /^[ \t]*```sw[ \t]*$/)) {
            in_block = 1
            indent = line
            sub(/```sw.*$/, "", indent)   # leading whitespace before ```
            start = NR
            n = 0
            delete buf
            next
        }
    } else {
        if (match(line, /^[ \t]*```[ \t]*$/)) {
            has_mod = 0; has_main = 0; body = ""
            for (k = 1; k <= n; k++) {
                b = buf[k]
                if (b ~ /^[ \t]*module[ \t]+[A-Za-z]/) has_mod = 1
                if (b ~ /^[ \t]*fun[ \t]+main[ \t]*\(/) has_main = 1
                body = body b "\n"
            }
            if (has_mod && has_main) {
                printf("@@BLOCK %d\n", start)
                printf("%s", body)
                printf("@@END\n")
            }
            in_block = 0
            next
        }
        stripped = line
        if (length(indent) > 0 && index(line, indent) == 1)
            stripped = substr(line, length(indent) + 1)
        n++
        buf[n] = stripped
        next
    }
}
AWK

echo '== Checking complete ```sw blocks in docs =='
for doc in "${DOC_FILES[@]}"; do
    if [ ! -f "$doc" ]; then
        echo "check_sw_docs: doc not found: $doc" >&2
        FAILURES=$((FAILURES + 1))
        continue
    fi
    echo "-- $doc"
    found_any=0
    # Stream extracted blocks; split on the @@BLOCK / @@END markers.
    block_start=""
    block_file=""
    while IFS= read -r mdline; do
        case "$mdline" in
            "@@BLOCK "*)
                block_start="${mdline#@@BLOCK }"
                block_file="$WORK/block.sw"
                : >"$block_file"
                found_any=1
                ;;
            "@@END")
                compile_check "$block_file" "$doc block @ line $block_start"
                block_start=""
                ;;
            *)
                if [ -n "$block_start" ]; then
                    printf '%s\n' "$mdline" >>"$block_file"
                fi
                ;;
        esac
    done < <(awk "$EXTRACT_AWK" "$doc")
    if [ "$found_any" -eq 0 ]; then
        echo "  (no complete-program sw blocks)"
    fi
done

echo
echo "== Compile-checking examples/*.sw with a main =="
shopt -s nullglob
for ex in examples/*.sw; do
    base="$(basename "$ex")"
    # Skip test files (swc test fodder, no main) and library-only files.
    case "$base" in
        *_test.sw) echo "  skip  $ex (test file)"; continue ;;
    esac
    if ! grep -Eq '^[[:space:]]*fun[[:space:]]+main[[:space:]]*\(' "$ex"; then
        echo "  skip  $ex (no main — library/fragment)"
        continue
    fi
    # Build in place so swc's relative auto-import (examples/<Mod>.sw,
    # e.g. multi_main.sw -> MathLib.sw) resolves.
    compile_check "$ex" "$ex"
done

echo
echo "============================================================"
if [ "$FAILURES" -gt 0 ]; then
    echo "check_sw_docs: $FAILURES program(s) failed to compile ($CHECKED ok)." >&2
    echo "check_sw_docs: a doc snippet or example has drifted from the language." >&2
    echo "check_sw_docs: fix the offending program(s) above (or the compiler)." >&2
    exit 1
fi
echo "check_sw_docs: all $CHECKED documented/example programs compile clean."
exit 0
