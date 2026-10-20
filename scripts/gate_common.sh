#!/usr/bin/env bash
# gate_common.sh — shared preflight for the stress and gate scripts (O8).
#
# Sourced by the gates (`. "$REPO/scripts/gate_common.sh"`), or run from a
# Makefile recipe as `bash scripts/gate_common.sh <helper> [args...]`.
#
# A gate's verdict can depend on host limits and installed tools. These helpers
# print the limits up front, and turn a missing prerequisite into a message that
# names the package to install, so it reads as an environment problem and not as
# a code failure. The need_* helpers print their message to stdout and return 1.

# Print the host limits a gate's verdict can depend on. Optional label prefix.
gate_print_limits() {
    local label="${1:-gate}" mmap="n/a"
    [ "$(uname)" = "Linux" ] && mmap=$(sysctl -n vm.max_map_count 2>/dev/null || echo '?')
    echo "[$label] host limits: vm.max_map_count=$mmap nofile=$(ulimit -n)" \
         "cores=$(ulimit -c) ulimit-v=$(ulimit -v)" \
         "cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo '?')"
}

# Require a command on PATH. <cmd> <install hint>
gate_need_cmd() {
    command -v "$1" >/dev/null 2>&1 && return 0
    echo "missing prerequisite: $1 (install: $2)"
    return 1
}

# Require a working ASAN+UBSAN link with <cc> (may be a multi-word command).
# On Linux a missing runtime only shows up as an ld "cannot find *.a" error
# that names no package, so probe once and name the package instead.
gate_need_asan() {
    local cc="$1" dir ver
    dir=$(mktemp -d "${TMPDIR:-/tmp}/gate_asan.XXXXXX") || return 1
    printf 'int main(void) { return 0; }\n' > "$dir/probe.c"
    if $cc -fsanitize=address,undefined "$dir/probe.c" -o "$dir/probe" >/dev/null 2>&1; then
        rm -rf "$dir"; return 0
    fi
    rm -rf "$dir"
    ver=$($cc -dumpversion 2>/dev/null | cut -d. -f1)
    if [ "$(uname)" != "Linux" ]; then
        echo "missing ASAN/UBSAN runtime for '$cc' (macOS: install the Xcode command-line tools, or brew install llvm)"
    elif $cc --version 2>/dev/null | grep -qi clang; then
        echo "missing ASAN/UBSAN runtime for '$cc': install libclang-rt-${ver}-dev (sudo apt-get install libclang-rt-${ver}-dev)"
    else
        echo "missing ASAN/UBSAN runtime for '$cc': install libasan${ver} and libubsan${ver} (sudo apt-get install libasan${ver} libubsan${ver})"
    fi
    return 1
}

# Executed directly rather than sourced: run the named helper with its args.
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ $# -gt 0 ]; then "$@"; fi
