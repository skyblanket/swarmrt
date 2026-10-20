#!/usr/bin/env bash
# Full CMake build, so a CMake-only break (a source missing from
# CMakeLists.txt, a link error only the CMake leg hits) fails the build
# instead of surfacing on the Windows/MinGW path. Runs from the Makefile's
# `check-cmake` target, which `make swc` and `make libswarmrt` depend on,
# so CI's Linux quickstart job exercises it.
#
# Linux only: the macOS leg does not run CMake. Without cmake, a local
# build skips with a warning; CI (GitHub Actions sets CI=true) fails.
set -euo pipefail
cd "$(dirname "$0")/.."

if [ "$(uname -s)" != "Linux" ]; then
    exit 0
fi

if ! command -v cmake >/dev/null 2>&1; then
    if [ -n "${CI:-}" ]; then
        echo "error: cmake not found; the CMake build check cannot run in CI" >&2
        exit 1
    fi
    echo "warning: cmake not found; skipping the CMake build check" >&2
    exit 0
fi

# bin/ is shared with the Makefile, and CMake writes swc there too. That is
# harmless: `make swc` links bin/swc after this check, so the Makefile's
# binary is the one that ends up in place.
build_dir=build/cmake
cmake -S . -B "$build_dir" > /dev/null
cmake --build "$build_dir" -j"$(nproc)"
echo "cmake build ok"
