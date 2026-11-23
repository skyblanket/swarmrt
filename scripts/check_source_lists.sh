#!/usr/bin/env bash
# The build's source lists live twice: in the Makefile and in CMakeLists.txt
# (the CMake path is the only Windows/MinGW build). They must name the same
# files, or one toolchain links while the other fails: swarmrt_varena.c was
# missing from CMake, so a CMake build could not link swc (#28).
#
# Compares, as sets of basenames without extension:
#   runtime library  Makefile CORE_SRCS          vs CMake CORE_SOURCES
#   swc compiler     Makefile `swc:` recipe srcs  vs CMake add_executable(swc ...)
# Run it with `make check-sources`. Fails loudly on drift or on a parse
# failure (an empty list is an error, never a pass).
set -euo pipefail
cd "$(dirname "$0")/.."

# Makefile CORE_SRCS = a b \ c d   (continued lines)
make_core=$(awk '/^CORE_SRCS[ \t]*=/{f=1} f{print} f && !/\\$/{exit}' Makefile \
    | sed -e 's/^CORE_SRCS[ \t]*=//' -e 's/\\$//' | tr -s ' \t' '\n' | grep -v '^$' | sort -u)
# The swc recipe: every $(SRC_DIR)/<file>.c up to the -o line.
make_swc=$(awk '/^swc:/{f=1; next} f && /^[^\t]/{exit} f{print} f && /-o /{exit}' Makefile \
    | grep -o '$(SRC_DIR)/[A-Za-z0-9_]*\.c' | sed -e 's|$(SRC_DIR)/||' -e 's/\.c$//' | sort -u)

cmake_core=$(awk '/^set\(CORE_SOURCES/{f=1; next} f && /^\)/{exit} f' CMakeLists.txt \
    | grep -o 'SRC_DIR}/[A-Za-z0-9_.]*' | sed -e 's|SRC_DIR}/||' -e 's/\.[cS]$//' | sort -u)
cmake_swc=$(awk '/^add_executable\(swc$/{f=1; next} f && /^\)/{exit} f' CMakeLists.txt \
    | grep -o 'SRC_DIR}/[A-Za-z0-9_.]*' | sed -e 's|SRC_DIR}/||' -e 's/\.c$//' | sort -u)

status=0
check() {
    local what=$1 a=$2 b=$3
    if [ -z "$a" ] || [ -z "$b" ]; then
        echo "error: could not extract the $what source list (Makefile or CMakeLists.txt changed shape?)" >&2
        status=1; return
    fi
    if ! diff <(echo "$a") <(echo "$b") > /dev/null; then
        echo "error: $what sources differ between the Makefile (<) and CMakeLists.txt (>):" >&2
        diff <(echo "$a") <(echo "$b") | grep '^[<>]' >&2 || true
        status=1; return
    fi
    echo "$what sources match: $(echo "$a" | wc -l | tr -d ' ') files"
}
check "runtime (CORE_SRCS / CORE_SOURCES)" "$make_core" "$cmake_core"
check "swc" "$make_swc" "$cmake_swc"
exit $status
