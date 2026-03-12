#!/bin/bash
# Build swarmrt-mcp for all platforms
# Usage: ./mcp/build-all.sh [version]
#
# Builds: darwin-arm64, darwin-x86_64, linux-x86_64, linux-arm64

set -e

VERSION="${1:-dev}"
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DIST_DIR="$ROOT_DIR/dist"
SRC="$ROOT_DIR/mcp/swarmrt_mcp.c"
SEARCH_SRC="$ROOT_DIR/src/swarmrt_search.c"
CFLAGS="-Wall -Wextra -Wno-unused-function -O2 -pthread -D_GNU_SOURCE -D_DARWIN_C_SOURCE"
LDFLAGS="-pthread -lm"

mkdir -p "$DIST_DIR"

echo "SwarmRT MCP Build — v$VERSION"
echo "=============================="

# Detect current platform
OS="$(uname -s)"
ARCH="$(uname -m)"

build_native() {
    local target="$1"
    local cc="${2:-cc}"
    local extra_cflags="${3:-}"
    local extra_ldflags="${4:-}"
    local outname="swarmrt-mcp-$target"

    echo ""
    echo "Building $outname..."

    # Compile search module
    $cc $CFLAGS $extra_cflags -c "$SEARCH_SRC" -o "$DIST_DIR/search_$target.o" 2>/dev/null || {
        echo "  SKIP (compiler not available for $target)"
        return 1
    }

    # Compile + link MCP server
    $cc $CFLAGS $extra_cflags "$DIST_DIR/search_$target.o" "$SRC" \
        -o "$DIST_DIR/$outname" $LDFLAGS $extra_ldflags 2>/dev/null || {
        echo "  SKIP (link failed for $target)"
        rm -f "$DIST_DIR/search_$target.o"
        return 1
    }

    # Cleanup obj
    rm -f "$DIST_DIR/search_$target.o"

    # Strip if not macOS (macOS strip works differently)
    if [[ "$target" == linux-* ]] && command -v strip &>/dev/null; then
        strip "$DIST_DIR/$outname" 2>/dev/null || true
    fi

    local size=$(wc -c < "$DIST_DIR/$outname" | tr -d ' ')
    echo "  OK: $DIST_DIR/$outname ($size bytes)"
    return 0
}

# Native build (current platform)
case "$OS-$ARCH" in
    Darwin-arm64)
        build_native "darwin-arm64" "cc" "-march=armv8-a" "-lz"
        # Try x86_64 cross-compile via Rosetta/lipo
        build_native "darwin-x86_64" "cc" "-target x86_64-apple-macos11" "-lz" || true
        ;;
    Darwin-x86_64)
        build_native "darwin-x86_64" "cc" "-march=x86-64" "-lz"
        build_native "darwin-arm64" "cc" "-target arm64-apple-macos11" "-lz" || true
        ;;
    Linux-x86_64)
        build_native "linux-x86_64" "cc" "-march=x86-64" "-lz"
        # Try cross-compile for arm64
        build_native "linux-arm64" "aarch64-linux-gnu-gcc" "" "-lz" || true
        ;;
    Linux-aarch64)
        build_native "linux-arm64" "cc" "" "-lz"
        build_native "linux-x86_64" "x86_64-linux-gnu-gcc" "" "-lz" || true
        ;;
    *)
        echo "Unknown platform: $OS-$ARCH"
        build_native "$OS-$ARCH" "cc" "" "-lz" || true
        ;;
esac

echo ""
echo "Build complete. Binaries in $DIST_DIR/"
ls -lh "$DIST_DIR"/swarmrt-mcp-* 2>/dev/null || echo "(no binaries built)"
