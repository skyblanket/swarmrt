#!/usr/bin/env bash
#
# doctest.sh — doc EXAMPLES must RUN and produce the OUTPUT they claim.
#
# check_sw_docs.sh proves every complete doc snippet COMPILES. This goes
# one step further: for every fenced ```sw block that is a complete
# program AND carries documented expected-output markers, it COMPILES +
# RUNS the program and ASSERTS that stdout matches, line for line, in
# order. A drift (the docs claim output the language no longer produces)
# fails the run non-zero. This is the tripwire for "the docs lie".
#
# --- Expected-output convention -------------------------------------------
# A doctest is a complete-program ```sw block (has both a `module` line
# and a `fun main(` line) that contains one or more `# => EXPECTED`
# comment markers. Each `# => X` line contributes ONE expected stdout
# line, collected top-to-bottom in source order. `# =>` is an ordinary
# sw comment, so the program still compiles and runs unchanged; the
# markers are stripped only for the expected-output transcript.
#
#   ```sw
#   module Demo
#   export [main]
#   fun main() {
#       print("hello")     # => hello
#       print(1 + 2)       # => 3
#   }
#   ```
#
# The marker may sit inline after code (`print(x)   # => 42`) or on its
# own line (`# => 42`). Leading/trailing whitespace around the expected
# text is trimmed on BOTH the marker and the actual stdout line before
# comparison, so indentation in the doc doesn't matter.
#
# A block with NO `# =>` markers is skipped here (check_sw_docs.sh still
# compile-checks it). Programs are run with SW_QUIET=1 so the runtime
# startup banner never pollutes the compared stdout.
#
# Run from anywhere:  bash scripts/doctest.sh
# Needs:  ./bin/swc  (build it first with `make swc libswarmrt`).

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

SWC="./bin/swc"
if [ ! -x "$SWC" ]; then
    echo "doctest: $SWC not found or not executable." >&2
    echo "doctest: build it first with: make swc libswarmrt" >&2
    exit 2
fi

# Docs whose ```sw blocks with `# =>` markers we run + assert.
DOC_FILES=(
    "docs/SW_LANGUAGE.md"
    "docs/AGENT_SYSTEM.md"
    "docs/BUILDING_AGENTS.md"
)

WORK="$(mktemp -d "${TMPDIR:-/tmp}/sw_doctest.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

FAILURES=0
RAN=0
ASSERTS=0

# --- awk extractor: emit each complete-program ```sw block that has at
# --- least one `# =>` marker as:
# ---   @@BLOCK <startline>
# ---   @@PROG
# ---   <full program source, markers kept (they're comments)>
# ---   @@EXPECT
# ---   <one expected line per # => marker, in order, trimmed>
# ---   @@END
# --- Leading indentation on the opening fence is stripped from the body.
read -r -d '' EXTRACT_AWK <<'AWK'
function trim(s) { gsub(/^[ \t]+/, "", s); gsub(/[ \t]+$/, "", s); return s }
BEGIN { in_block = 0 }
{
    line = $0
    if (in_block == 0) {
        if (match(line, /^[ \t]*```sw[ \t]*$/)) {
            in_block = 1
            indent = line
            sub(/```sw.*$/, "", indent)
            start = NR
            n = 0
            delete buf
            next
        }
    } else {
        if (match(line, /^[ \t]*```[ \t]*$/)) {
            has_mod = 0; has_main = 0; nexpect = 0
            delete expect
            for (k = 1; k <= n; k++) {
                b = buf[k]
                if (b ~ /^[ \t]*module[ \t]+[A-Za-z]/) has_mod = 1
                if (b ~ /^[ \t]*fun[ \t]+main[ \t]*\(/) has_main = 1
                # Capture the text after a `# =>` marker as an expected line.
                if (match(b, /#[ \t]*=>[ \t]?/)) {
                    expline = substr(b, RSTART + RLENGTH)
                    nexpect++
                    expect[nexpect] = trim(expline)
                }
            }
            if (has_mod && has_main && nexpect > 0) {
                printf("@@BLOCK %d\n", start)
                printf("@@PROG\n")
                for (k = 1; k <= n; k++) printf("%s\n", buf[k])
                printf("@@EXPECT\n")
                for (k = 1; k <= nexpect; k++) printf("%s\n", expect[k])
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

# Run one doctest. $1 = program file, $2 = expected file, $3 = label.
run_doctest() {
    local prog="$1" expect="$2" label="$3"
    local bin="$WORK/dt.bin" log="$WORK/dt.log" out="$WORK/dt.out"

    if ! "$SWC" build "$prog" -o "$bin" >"$log" 2>&1; then
        FAILURES=$((FAILURES + 1))
        echo "  FAIL  $label (compile)" >&2
        sed 's/^/  | /' "$log" >&2
        return
    fi

    # SW_QUIET silences the startup banner. perl alarm bounds the run
    # (macOS has no `timeout`). Capture stdout only; stderr is diagnostics.
    SW_QUIET=1 perl -e 'alarm 10; exec @ARGV or die' "$bin" >"$out" 2>"$log"
    local rc=$?
    rm -f "$bin"

    # Trim each actual stdout line for comparison (mirrors marker trim).
    local actual_trimmed="$WORK/dt.actual"
    sed 's/^[ \t]*//; s/[ \t]*$//' "$out" >"$actual_trimmed"
    # The expected file is already trimmed by the awk extractor.

    if diff -q "$expect" "$actual_trimmed" >/dev/null 2>&1; then
        RAN=$((RAN + 1))
        local nlines
        nlines=$(wc -l <"$expect" | tr -d ' ')
        ASSERTS=$((ASSERTS + nlines))
        echo "  ok    $label ($nlines line(s) asserted)"
    else
        FAILURES=$((FAILURES + 1))
        echo "  FAIL  $label (output drift, exit=$rc)" >&2
        echo "  ----- expected (from # => markers) -------------------------" >&2
        sed 's/^/  | /' "$expect" >&2
        echo "  ----- actual stdout ----------------------------------------" >&2
        sed 's/^/  | /' "$actual_trimmed" >&2
        if [ -s "$log" ]; then
            echo "  ----- stderr -----------------------------------------------" >&2
            sed 's/^/  | /' "$log" >&2
        fi
        echo "  ------------------------------------------------------------" >&2
    fi
}

echo '== Doctesting complete `sw` blocks with # => markers =='
for doc in "${DOC_FILES[@]}"; do
    if [ ! -f "$doc" ]; then
        echo "doctest: doc not found: $doc" >&2
        FAILURES=$((FAILURES + 1))
        continue
    fi
    echo "-- $doc"
    found_any=0
    mode=""
    block_start=""
    prog_file="$WORK/prog.sw"
    expect_file="$WORK/expect.txt"
    while IFS= read -r mdline; do
        case "$mdline" in
            "@@BLOCK "*)
                block_start="${mdline#@@BLOCK }"
                : >"$prog_file"
                : >"$expect_file"
                found_any=1
                mode=""
                ;;
            "@@PROG")  mode="prog" ;;
            "@@EXPECT") mode="expect" ;;
            "@@END")
                run_doctest "$prog_file" "$expect_file" "$doc block @ line $block_start"
                mode=""
                ;;
            *)
                case "$mode" in
                    prog)   printf '%s\n' "$mdline" >>"$prog_file" ;;
                    expect) printf '%s\n' "$mdline" >>"$expect_file" ;;
                esac
                ;;
        esac
    done < <(awk "$EXTRACT_AWK" "$doc")
    if [ "$found_any" -eq 0 ]; then
        echo "  (no doctest blocks — no complete program carries # => markers)"
    fi
done

echo
echo "============================================================"
if [ "$FAILURES" -gt 0 ]; then
    echo "doctest: $FAILURES doctest(s) failed ($RAN ran, $ASSERTS line(s) asserted)." >&2
    echo "doctest: a doc example's claimed output has drifted from the language." >&2
    echo "doctest: fix the example, the # => marker, or the compiler." >&2
    exit 1
fi
echo "doctest: all $RAN doctest(s) ran and matched — $ASSERTS output line(s) asserted."
exit 0
