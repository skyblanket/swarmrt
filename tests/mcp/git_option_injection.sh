#!/bin/sh
# Regression test (#109): a `ref` (or other git argument) that starts with
# `-` reached git as an option. git_diff with ref `--output=<file>` made git
# write that file. Expect: the call is refused, no file appears, and an
# ordinary ref still works.
#
# Usage: tests/mcp/git_option_injection.sh [path/to/swarmrt-mcp]

set -eu

BIN=${1:-bin/swarmrt-mcp}
[ -x "$BIN" ] || { echo "missing MCP binary: $BIN" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

ROOT="$WORK/proj"
OUTFILE="$WORK/written-by-git"
mkdir -p "$ROOT"
(
    cd "$ROOT"
    git init -q
    echo a > f.txt
    git add f.txt
    git -c user.email=t@example.invalid -c user.name=t commit -qm init
    echo changed >> f.txt
)

OUT=$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"git_diff\",\"arguments\":{\"ref\":\"--output=$OUTFILE\"}}}" \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"git_diff","arguments":{"ref":"HEAD"}}}' \
    | timeout 10 "$BIN" "$ROOT" 2>/dev/null || true)

if [ -e "$OUTFILE" ]; then
    echo "FAIL: git_diff passed a ref starting with '-' to git as an option" >&2
    exit 1
fi

case "$OUT" in
    *'"id":2'*'Invalid ref'*) ;;
    *) echo "FAIL: git_diff did not refuse the option-shaped ref" >&2
       echo "$OUT" >&2
       exit 1 ;;
esac

case "$OUT" in
    *'"id":3'*'f.txt'*) ;;
    *) echo "FAIL: git_diff with ref HEAD did not report f.txt" >&2
       echo "$OUT" >&2
       exit 1 ;;
esac

echo "OK git_option_injection"
