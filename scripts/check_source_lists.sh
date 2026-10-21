#!/usr/bin/env bash
# The runtime source list lives twice: CORE_SRCS in the Makefile and
# CORE_SOURCES in CMakeLists.txt. They must name the same files, or a
# build on one toolchain links while the other fails (swarmrt_varena was
# missing from CMake and `swc` failed to link). Fails if they drift.
set -euo pipefail
cd "$(dirname "$0")/.."

# Basenames without extension, one per line.
make_list=$(awk '/^CORE_SRCS =/{f=1} f{print} f && !/\\$/{exit}' Makefile \
    | sed -e 's/^CORE_SRCS =//' -e 's/\\$//' | tr -s ' \t' '\n' | grep -v '^$' | sort -u)
cmake_list=$(awk '/^set\(CORE_SOURCES/{f=1; next} f && /^\)/{exit} f' CMakeLists.txt \
    | grep -o 'SRC_DIR}/[^ )]*' | sed -e 's|SRC_DIR}/||' -e 's/\.[cS]$//' | sort -u)

if [ -z "$make_list" ] || [ -z "$cmake_list" ]; then
    echo "error: could not extract CORE_SRCS / CORE_SOURCES" >&2
    exit 1
fi

if ! diff <(echo "$make_list") <(echo "$cmake_list"); then
    echo "error: Makefile CORE_SRCS and CMakeLists.txt CORE_SOURCES differ (see diff above)" >&2
    exit 1
fi
echo "source lists match: $(echo "$make_list" | wc -l) runtime files"
