#!/usr/bin/env bash
# Full CMake build plus a smoke run of what it produced, so a CMake-only break
# (a source missing from CMakeLists.txt, a link error only that path hits, a
# runtime archive swc cannot find) fails here instead of on the Windows/MinGW
# path, which is CMake-only (#28). Run it with `make check-cmake`.
#
# Outputs go under build/cmake, never bin/: bin/ holds what the Makefile
# built. build/cmake/root is laid out like an install (bin/ plus src/ and
# lib/ symlinked back to the checkout) because swc finds its headers, the
# runtime archive and the stdlib relative to its own binary.
#
# Without cmake this fails rather than skipping: a skipped check reads as a
# passing one.
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found; install cmake to run the CMake build check" >&2
    exit 1
fi

build_dir=build/cmake
root="$PWD/$build_dir/root"
mkdir -p "$root/bin"
ln -sfn "$PWD/src" "$root/src"
ln -sfn "$PWD/lib" "$root/lib"

cmake -S . -B "$build_dir/obj" -DSWARMRT_BIN_DIR="$root/bin" > /dev/null
jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
cmake --build "$build_dir/obj" -j"$jobs"

swc="$root/bin/swc"
fail() { echo "error: cmake smoke: $*" >&2; exit 1; }

# 1. version is wired from VERSION, not the 0.0.0-dev fallback
want=$(head -1 VERSION)
"$swc" --version | grep -qF "$want" || fail "swc --version ($("$swc" --version)) does not carry VERSION $want"

# 2. the interpreter runs a program
out=$("$swc" run examples/hello.sw) || fail "swc run examples/hello.sw exited non-zero"
[ "$out" = "Hello, Swarm!" ] || fail "swc run printed '$out', want 'Hello, Swarm!'"

# 3. a compiled program links against the CMake-built libswarmrt.a and runs
exe="$build_dir/hello"
"$swc" build examples/hello.sw -o "$exe" || fail "swc build examples/hello.sw failed"
out=$("$exe") || fail "compiled hello exited non-zero"
[ "$out" = "Hello, Swarm!" ] || fail "compiled hello printed '$out', want 'Hello, Swarm!'"

echo "cmake build ok: swc $want builds, runs and links compiled programs"
