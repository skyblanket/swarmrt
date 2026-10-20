#!/bin/sh
# Regression test: the MCP server's git-aware autopilot tools must not pass
# the project root through a shell unquoted. A root containing `'` and a
# `$(touch ...)` payload used to break out of the `cd '...'` quoting, run the
# payload, and lose the diff. Expect: no canary, and the modified file listed.
#
# Usage: tests/mcp/project_root_quoting.sh [path/to/swarmrt-mcp]

set -eu

BIN=${1:-bin/swarmrt-mcp}
[ -x "$BIN" ] || { echo "missing MCP binary: $BIN" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

CANARY="$WORK/pwned"
ROOT="$WORK/proj'\$(touch $CANARY)'x"

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
    '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"autopilot_start","arguments":{"goal":"g","steps":["a"]}}}' \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"autopilot_status","arguments":{}}}' \
    '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"git_diff","arguments":{}}}' \
    '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"git_log","arguments":{}}}' \
    '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"codebase_overview","arguments":{}}}' \
    | timeout 10 "$BIN" "$ROOT" 2>/dev/null || true)

if [ -e "$CANARY" ]; then
    echo "FAIL: project root payload was executed by the shell" >&2
    exit 1
fi

case "$OUT" in
    *'dirty_files'*'f.txt'*) ;;
    *) echo "FAIL: autopilot_status did not report f.txt as dirty" >&2
       echo "$OUT" >&2
       exit 1 ;;
esac

case "$OUT" in
    *'"id":4'*'f.txt'*) ;;
    *) echo "FAIL: git_diff did not report f.txt" >&2
       echo "$OUT" >&2
       exit 1 ;;
esac

case "$OUT" in
    *'"id":5'*'init'*) ;;
    *) echo "FAIL: git_log did not report the init commit" >&2
       echo "$OUT" >&2
       exit 1 ;;
esac

case "$OUT" in
    *'"id":6'*'git_head'*) ;;
    *) echo "FAIL: codebase_overview did not report git_head" >&2
       echo "$OUT" >&2
       exit 1 ;;
esac

echo "ok: project root with quote + command substitution is inert and diff works"
