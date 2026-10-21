#!/usr/bin/env bash
# Full CMake build, so a CMake-only break (a source missing from
# CMakeLists.txt, a link error only the CMake leg hits) is caught here
# instead of on the Windows/MinGW path. Run it with `make check-cmake`.
#
# Linux only: the macOS leg does not run CMake. Without cmake this fails
# rather than skipping, because a skipped check looks like a passing one.
set -euo pipefail
cd "$(dirname "$0")/.."

if [ "$(uname -s)" != "Linux" ]; then
    exit 0
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found; install cmake to run the CMake build check" >&2
    exit 1
fi

# CMake's outputs go under build/cmake, not bin/: bin/ holds the binaries the
# Makefile built, and the test suite runs them after this check.
build_dir=build/cmake
cmake -S . -B "$build_dir" -DSWARMRT_BIN_DIR="$PWD/$build_dir/bin" > /dev/null
cmake --build "$build_dir" -j"$(nproc)"
echo "cmake build ok"
