#!/bin/bash
# eval/check_leakage.sh — answer-leakage gate for the sw eval.
#
# The eval is only honest if the system prompt teaches the LANGUAGE, not the
# ANSWERS. It is trivially easy to "win" a benchmark by pasting each task's
# worked solution into the shared system prompt — every model then just
# transcribes it. This script is the invariant that keeps that from happening
# (or from silently regressing).
#
# What it does:
#   For each task in prompts/, it takes that task's canonical solution (the
#   reference sw program that produces the expected output), reduces it to a
#   stream of *code tokens* (comments/whitespace stripped), and slides a
#   K-gram window (default 5) over those tokens. Each K-gram is searched, as a
#   contiguous token sequence, against the same tokenized form of
#   system_prompt.md. If a task's solution K-gram appears in the prompt, the
#   answer has leaked: we print the offending task, the matching K-gram, and
#   the prompt line it landed on, then EXIT NONZERO so CI fails.
#
# Why K-grams of tokens (not raw substrings / not single tokens):
#   - Single shared tokens ("print", "module", "Std.sum") are legitimate
#     language vocabulary and appear in any honest reference — matching those
#     would be all false positives.
#   - A contiguous run of 5 code tokens lifted verbatim from a task's solution
#     is not vocabulary; it is the answer. Tokenizing first means cosmetic
#     reformatting (indentation, comments, line breaks) cannot hide a leak.
#
# This script depends on NOTHING the de-leak agent is editing except reading
# system_prompt.md. It passes the moment the worked task-solutions are stripped
# from the prompt, and stays correct regardless of when that happens.
#
# Usage:
#   ./check_leakage.sh            # check every task
#   ./check_leakage.sh 05 09      # check only tasks whose id contains 05 / 09
#   NGRAM=6 ./check_leakage.sh    # require longer runs to flag (less strict)
#
# Exit: 0 = clean (no task solution found in the prompt); 1 = leak detected;
#       2 = setup error (missing files, no tasks).

set -u
cd "$(dirname "$0")"

PROMPT_FILE="system_prompt.md"
PROMPTS_DIR="prompts"
NGRAM="${NGRAM:-5}"

case "$NGRAM" in
    ''|*[!0-9]*) echo "error: NGRAM must be a positive integer (got '$NGRAM')" >&2; exit 2 ;;
esac
[ "$NGRAM" -lt 2 ] && NGRAM=2

if [ ! -f "$PROMPT_FILE" ]; then
    echo "error: $PROMPT_FILE not found (run from eval/ or its parent)" >&2
    exit 2
fi

if [ -t 1 ]; then
    GREEN=$'\e[32m'; RED=$'\e[31m'; YEL=$'\e[33m'; DIM=$'\e[2m'; BOLD=$'\e[1m'; RESET=$'\e[0m'
else
    GREEN=''; RED=''; YEL=''; DIM=''; BOLD=''; RESET=''
fi

# ---------------------------------------------------------------------------
# Canonical solutions.
#
# The prompt .md files only carry the task statement and the expected *stdout*
# — not the reference program. So the canonical solution for each task lives
# here, as the single source of truth for "what counts as the answer". These
# are exactly the kind of worked programs that must NOT appear in the shared
# system prompt; embedding them here lets the check assert that invariant
# without creating solution files elsewhere in the tree.
#
# To add a task: add a case arm keyed by the task id prefix (the leading
# NN_name from prompts/NN_name.md). Keep it to the minimal idiomatic program
# that produces the prompt's Expected output. Anything that is purely generic
# language vocabulary (a lone `print`, a bare `import Std`) will not, on its
# own, form a distinctive K-gram, so the check stays free of false positives.
# ---------------------------------------------------------------------------
canonical_solution() {
    case "$1" in
    01_hello_world)
        cat <<'SW'
module Main
fun main() {
    print("hello, world")
}
SW
        ;;
    02_fizzbuzz)
        cat <<'SW'
module Main
import Std
fun fb(n) {
    if (n % 15 == 0) { print("FizzBuzz") }
    else { if (n % 3 == 0) { print("Fizz") }
    else { if (n % 5 == 0) { print("Buzz") }
    else { print(n) } } }
}
fun main() {
    for i in Std.range(1, 16) { fb(i) }
}
SW
        ;;
    03_list_sum)
        cat <<'SW'
module Main
import Std
fun main() {
    xs = [1, 2, 3]
    total = Std.sum(xs)
    print(f"sum={total}")
}
SW
        ;;
    04_json_parse)
        cat <<'SW'
module Main
fun main() {
    data = json_decode("{\"name\": \"sw\", \"count\": 3}")
    name = map_get(data, "name")
    count = map_get(data, "count")
    print(f"{name}:{count}")
}
SW
        ;;
    05_actor_counter)
        cat <<'SW'
module Main
fun counter(n) {
    receive {
        'incr'          -> counter(n + 1)
        {'get', caller} -> {
            send(caller, n)
            counter(n)
        }
    }
}
fun main() {
    pid = spawn(fun() { counter(0) })
    send(pid, 'incr')
    send(pid, 'incr')
    send(pid, 'incr')
    send(pid, 'incr')
    send(pid, 'incr')
    send(pid, {'get', self()})
    receive {
        count -> print(f"count={count}")
    }
}
SW
        ;;
    06_case_dispatch)
        cat <<'SW'
module Main
fun classify(val) {
    case val {
        _ when typeof(val) == "int"  -> "int"
        []                           -> "empty_list"
        _ when typeof(val) == "list" -> f"list:{length(val)}"
        {'ok', v}                    -> f"ok:{v}"
        _                            -> "other"
    }
}
fun main() {
    print(classify(42))
}
SW
        ;;
    07_sqlite_crud)
        cat <<'SW'
module Main
fun main() {
    db = db_open(":memory:")
    db_exec(db, "CREATE TABLE t (id INTEGER, name TEXT)")
    db_exec(db, "INSERT INTO t VALUES (1, 'a')")
    rows = db_query(db, "SELECT name FROM t WHERE id = ?", [1])
    row = hd(rows)
    print(map_get(row, "name"))
}
SW
        ;;
    08_pipe_filter)
        cat <<'SW'
module Main
import Std
fun main() {
    result = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
        |> filter(fun(x) { x % 2 == 0 })
        |> map(fun(x) { x * x })
        |> Std.sum()
    print(result)
}
SW
        ;;
    09_fault_tolerance)
        cat <<'SW'
module Main
fun bad() { panic("boom") }
fun main() {
    trap_exit('true')
    pid = spawn(fun() { bad() })
    link(pid)
    receive {
        {'EXIT', from, reason} -> print("parent_survived")
    }
}
SW
        ;;
    10_http_pipeline)
        cat <<'SW'
module Main
import Std
fun main() {
    body = http_get("https://example.com/data.json")
    data = json_decode(body)
    items = map_get(data, "items")
    total = Std.sum([map_get(i, "value") for i in items])
    print(f"total={total}")
}
SW
        ;;
    *)
        # Unknown task: no canonical solution registered. Emit nothing; the
        # caller treats an empty solution as "nothing to check" (and warns).
        return 0
        ;;
    esac
}

# ---------------------------------------------------------------------------
# Tokenizer: collapse sw source (or the prompt) into a single line of code
# tokens separated by U+0020, with line-fenced reconstruction available via a
# parallel per-token line map. We:
#   - drop `#` line comments,
#   - split on runs of non-token characters,
#   - keep identifiers/keywords (incl. dots like Std.sum), numbers, strings'
#     inner words, and standalone operators.
# The point is structural equivalence: `print ( "x" )` and `print("x")` reduce
# to the same token stream, so reformatting can't mask a lifted answer.
#
# Output: space-separated tokens on STDOUT.
# ---------------------------------------------------------------------------
tokenize() {
    # Reads source on STDIN, writes one space-joined token stream to STDOUT.
    awk '
        {
            line = $0
            sub(/#.*/, "", line)              # strip line comments
            # turn every non [A-Za-z0-9._] run into a single space, so that
            # operators/braces/quotes become separators. Dotted names
            # (Std.sum) and floats stay intact.
            gsub(/[^A-Za-z0-9._]+/, " ", line)
            n = split(line, a, " ")
            for (i = 1; i <= n; i++) {
                t = a[i]
                if (t == "" ) continue
                # drop tokens that are only dots/underscores/dots-runs
                if (t ~ /^[._]+$/) continue
                printf "%s ", t
            }
        }
    '
}

# Produce, for the prompt, a token -> source-line index so we can report WHERE
# a leaked K-gram landed. We emit "lineno<TAB>token" pairs.
tokenize_with_lines() {
    awk '
        {
            ln = NR
            line = $0
            sub(/#.*/, "", line)
            gsub(/[^A-Za-z0-9._]+/, " ", line)
            n = split(line, a, " ")
            for (i = 1; i <= n; i++) {
                t = a[i]
                if (t == "") continue
                if (t ~ /^[._]+$/) continue
                printf "%d\t%s\n", ln, t
            }
        }
    '
}

# ---------------------------------------------------------------------------
# Distinctive (answer-defining) tokens for a task.
#
# Scaffolding (`module Main`, `import Std`, `fun main`, `pid = spawn(...)`,
# `receive { ... }`) is shared by every actor/pipe/fault-tolerance program and
# is legitimately taught as language SHAPE in a de-leaked prompt. What makes a
# program the ANSWER to a *specific* task is its task-specific literals: the
# string contents it prints, the atoms it matches on, the exact constants. We
# extract those from (a) the string literals inside the canonical solution and
# (b) the task's "Expected output" block in prompts/<id>.md.
#
# A candidate n-gram is only treated as a leak if it contains at least one of
# these distinctive tokens. That is the difference between "the prompt teaches
# spawn/receive" (fine) and "the prompt contains `print(\"parent_survived\")`
# and `panic(\"boom\")`" (a leaked answer).
#
# Emits space-separated distinctive tokens on STDOUT (may be empty).
# ---------------------------------------------------------------------------
distinctive_tokens() {
    local id="$1"
    local sol="$2"
    {
        # (a) words inside string literals "..." in the canonical solution —
        #     these are what the program PRINTS / matches on (`hello, world`,
        #     `parent_survived`, `FizzBuzz`, ...).
        echo "$sol" | grep -oE '"[^"]*"' | tr -d '"' \
            | tr -c 'A-Za-z0-9_' ' '
        # (b) tokens of the prompt's Expected output block — the verbatim
        #     stdout that DEFINES correctness for this task (`count=5`,
        #     `parent_survived`, `220`, `FizzBuzz`, `akash:3`, ...). This is
        #     the authoritative answer fingerprint, independent of however the
        #     canonical solution happens to be written.
        if [ -f "$PROMPTS_DIR/$id.md" ]; then
            awk '
                /^## Expected output/ { in_exp = 1; next }
                in_exp && /^```/      { in_block = !in_block; next }
                in_exp && in_block    { print }
                in_exp && /^## / && !/^## Expected output/ { exit }
            ' "$PROMPTS_DIR/$id.md"
        fi
        # NOTE: solution *atoms* (`'incr'`, `'EXIT'`, `'set'`, `'DOWN'`) are
        # deliberately NOT used as distinctive tokens — they are shared process
        # /protocol vocabulary that an honest prompt teaches as API shape
        # (`{'EXIT', from, reason}`, `trap_exit('true')`). The correctness-
        # defining signal lives in the printed strings (a) and expected
        # output (b).
    } | tokenize | tr ' ' '\n' \
        | grep -vE '^[0-9]+$' \
        | grep -vE '^(module|Main|Hello|import|Std|fun|main|print|return|x|n|i|v|caller|value|reason|from|pid|true|false|ok|error|nil)$' \
        | sort -u | tr '\n' ' '
    # NOTE: dropped on purpose —
    #   * bare integers: a digit run like `1 2 3 4 5` in a `Std.range` syntax
    #     demo is not an answer; correctness-defining *strings/atoms* are.
    #   * boolean/result atoms (`'true'`, `'false'`, `'ok'`, `'error'`, `'nil'`)
    #     are universal sw vocabulary — `trap_exit('true')` teaches the API,
    #     it does not leak a task's answer. The answer-defining atom for the
    #     fault-tolerance task is the printed string `parent_survived`, which
    #     IS retained.
}

# True iff the space-joined n-gram contains at least one distinctive token.
gram_has_distinctive() {
    local gram="$1"; local distinct="$2"
    local t d
    for t in $gram; do
        for d in $distinct; do
            [ "$t" = "$d" ] && return 0
        done
    done
    return 1
}

# ---------------------------------------------------------------------------
# Pre-tokenize the prompt once. PROMPT_TOKENS is the flat stream (space-joined,
# leading+trailing single spaces) for fast substring containment; PROMPT_MAP is
# the lineno<TAB>token form for locating a hit.
# ---------------------------------------------------------------------------
PROMPT_TOKENS=" $(tokenize < "$PROMPT_FILE") "
PROMPT_MAP="$(tokenize_with_lines < "$PROMPT_FILE")"

# Find the source line in system_prompt.md where a given space-joined K-gram
# first occurs as a contiguous token run. Returns "lineno: <raw source line>".
locate_hit() {
    local gram="$1"
    local k
    k=$(echo "$gram" | wc -w | tr -d ' ')
    # Walk the token/line map; whenever a window of k tokens starting here
    # equals the gram, report the source line of the window's first token.
    awk -v gram="$gram" -v k="$k" '
        { ln[NR] = $1; tok[NR] = $2 }
        END {
            split(gram, want, " ")
            total = NR
            for (s = 1; s + k - 1 <= total; s++) {
                ok = 1
                for (j = 1; j <= k; j++) {
                    if (tok[s + j - 1] != want[j]) { ok = 0; break }
                }
                if (ok) { print ln[s]; exit }
            }
        }
    ' <<< "$PROMPT_MAP"
}

# ---------------------------------------------------------------------------
# Select tasks: all prompts/*.md, or those whose id matches an arg substring.
# ---------------------------------------------------------------------------
all_ids=()
for f in "$PROMPTS_DIR"/*.md; do
    [ -e "$f" ] || continue
    all_ids+=("$(basename "$f" .md)")
done

if [ "${#all_ids[@]}" -eq 0 ]; then
    echo "error: no prompts found in $PROMPTS_DIR/" >&2
    exit 2
fi

selected=()
if [ "$#" -gt 0 ]; then
    for id in "${all_ids[@]}"; do
        for pat in "$@"; do
            case "$id" in *"$pat"*) selected+=("$id"); break ;; esac
        done
    done
    if [ "${#selected[@]}" -eq 0 ]; then
        echo "error: no task id matched: $*" >&2
        exit 2
    fi
else
    selected=("${all_ids[@]}")
fi

# ---------------------------------------------------------------------------
# Generic-vocabulary filter.
#
# A handful of tokens are universal sw scaffolding that a HONEST, de-leaked
# system prompt is expected and encouraged to contain (the mandatory
# `module Name` header, `import Std`, the `fun main()` entry point, `print`,
# the canonical `module Hello` smoke-test skeleton, etc.). An n-gram built
# only from these tokens is boilerplate, not an answer — flagging it would
# make the gate impossible to ever pass even after the worked solutions are
# stripped.
#
# So a candidate n-gram counts as a leak ONLY if it contains at least one
# token outside this generic set. Distinctive tokens are what carry a task's
# actual answer: literal-string words, task identifiers, and non-trivial
# builtins (spawn/receive/trap_exit/http_get/db_query/json_decode/...).
# ---------------------------------------------------------------------------
is_generic_token() {
    case "$1" in
        module|Main|Hello|import|Std|fun|main|print|return|0|1|2|3|x|n|i)
            return 0 ;;
        *) return 1 ;;
    esac
}

# True iff EVERY token in the space-joined n-gram is generic vocabulary.
gram_all_generic() {
    local t
    for t in $1; do
        is_generic_token "$t" || return 1
    done
    return 0
}

echo "${BOLD}== sw eval — answer-leakage check (${NGRAM}-grams) ==${RESET}"
echo "prompt: $PROMPT_FILE"
echo "tasks:  ${#selected[@]}"
echo ""

leaked=0          # number of tasks whose solution leaked
no_canon=0        # tasks with no registered canonical solution

for id in "${selected[@]}"; do
    sol="$(canonical_solution "$id")"
    if [ -z "$sol" ]; then
        echo "  ${YEL}WARN${RESET}  $id ${DIM}(no canonical solution registered — skipped)${RESET}"
        no_canon=$((no_canon + 1))
        continue
    fi

    # Tokenize this task's canonical solution into an array.
    sol_tokens="$(echo "$sol" | tokenize)"
    # shellcheck disable=SC2206
    toks=($sol_tokens)
    ntok=${#toks[@]}

    # Answer-defining tokens for this task. If a task somehow has none (all
    # of its content is generic scaffolding), there is nothing distinctive to
    # leak and we won't flag it.
    distinct="$(distinctive_tokens "$id" "$sol")"

    if [ "$ntok" -lt "$NGRAM" ]; then
        # Solution shorter than the window: shrink the window to its length so
        # we still test the whole thing as one gram.
        window="$ntok"
    else
        window="$NGRAM"
    fi
    [ "$window" -lt 1 ] && window=1

    hit_gram=""
    # Slide a window of size `window` over the solution tokens; the first
    # K-gram that appears verbatim in the prompt token stream is a leak.
    last_start=$((ntok - window))
    [ "$last_start" -lt 0 ] && last_start=0
    i=0
    while [ "$i" -le "$last_start" ]; do
        gram=""
        j=0
        while [ "$j" -lt "$window" ]; do
            idx=$((i + j))
            if [ -z "$gram" ]; then gram="${toks[$idx]}"; else gram="$gram ${toks[$idx]}"; fi
            j=$((j + 1))
        done
        # Skip n-grams that are purely generic sw scaffolding — a de-leaked
        # prompt legitimately contains those, so they must never count.
        if gram_all_generic "$gram"; then
            i=$((i + 1))
            continue
        fi
        # A leak n-gram must carry at least one answer-defining (distinctive)
        # token. A contiguous run of pure shape/scaffold ( `fun main pid spawn
        # fun`, `import Std fun main result` ) is taught legitimately and is
        # NOT the answer to this specific task.
        if ! gram_has_distinctive "$gram" "$distinct"; then
            i=$((i + 1))
            continue
        fi
        # Contiguous-token containment: surround with spaces so we only match
        # whole-token runs, never partial-token coincidences.
        case "$PROMPT_TOKENS" in
            *" $gram "*) hit_gram="$gram"; break ;;
        esac
        i=$((i + 1))
    done

    if [ -n "$hit_gram" ]; then
        leaked=$((leaked + 1))
        loc="$(locate_hit "$hit_gram")"
        echo "  ${RED}LEAK${RESET}  $id"
        echo "        ${DIM}solution ${NGRAM}-gram found verbatim in $PROMPT_FILE:${RESET}"
        echo "        ${RED}${hit_gram}${RESET}"
        if [ -n "$loc" ]; then
            src_line="$(sed -n "${loc}p" "$PROMPT_FILE")"
            echo "        ${DIM}at $PROMPT_FILE:${loc}:${RESET} ${YEL}${src_line}${RESET}"
        fi
    else
        echo "  ${GREEN}OK${RESET}    $id ${DIM}(no solution ${NGRAM}-gram in prompt)${RESET}"
    fi
done

echo ""
if [ "$no_canon" -gt 0 ]; then
    echo "${YEL}note:${RESET} $no_canon task(s) had no canonical solution registered and were skipped."
    echo "      add a case arm in canonical_solution() so they are gated too."
fi

if [ "$leaked" -gt 0 ]; then
    echo "${RED}${BOLD}FAIL:${RESET} $leaked task solution(s) leaked into $PROMPT_FILE."
    echo "The system prompt is teaching answers, not the language. Strip the worked"
    echo "task solutions from $PROMPT_FILE (keep only generic language reference),"
    echo "then re-run this check."
    exit 1
fi

echo "${GREEN}${BOLD}PASS:${RESET} no task solution leaked into $PROMPT_FILE."
exit 0
