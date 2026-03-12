#!/bin/bash
# SwarmRT MCP — Universal Installer
# curl -fsSL https://raw.githubusercontent.com/skyblanket/swarmrt/main/mcp/get.sh | bash
#
# Detects OS/arch, downloads the right binary, installs to ~/.local/bin,
# and adds to Claude Code.

set -e

REPO="skyblanket/swarmrt"
INSTALL_DIR="${SWARMRT_INSTALL_DIR:-$HOME/.local/bin}"
BINARY_NAME="swarmrt-mcp"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
DIM='\033[2m'
RESET='\033[0m'

info()  { echo -e "${GREEN}$1${RESET}"; }
dim()   { echo -e "${DIM}$1${RESET}"; }
error() { echo -e "${RED}Error: $1${RESET}" >&2; exit 1; }

# Detect platform
detect_platform() {
    local os arch
    os="$(uname -s)"
    arch="$(uname -m)"

    case "$os" in
        Darwin) os="darwin" ;;
        Linux)  os="linux" ;;
        *)      error "Unsupported OS: $os. Only macOS and Linux are supported." ;;
    esac

    case "$arch" in
        x86_64|amd64)   arch="x86_64" ;;
        arm64|aarch64)   arch="arm64" ;;
        *)               error "Unsupported architecture: $arch" ;;
    esac

    echo "${os}-${arch}"
}

# Get latest release tag
get_latest_version() {
    local url="https://api.github.com/repos/$REPO/releases/latest"
    if command -v curl &>/dev/null; then
        curl -fsSL "$url" 2>/dev/null | grep '"tag_name"' | head -1 | sed 's/.*"tag_name": *"//;s/".*//'
    elif command -v wget &>/dev/null; then
        wget -qO- "$url" 2>/dev/null | grep '"tag_name"' | head -1 | sed 's/.*"tag_name": *"//;s/".*//'
    fi
}

# Download binary
download() {
    local url="$1" dest="$2"
    if command -v curl &>/dev/null; then
        curl -fsSL -o "$dest" "$url"
    elif command -v wget &>/dev/null; then
        wget -qO "$dest" "$url"
    else
        error "Neither curl nor wget found. Install one and try again."
    fi
}

main() {
    echo ""
    info "SwarmRT MCP Installer"
    echo "====================="
    echo ""

    # Detect platform
    local platform
    platform="$(detect_platform)"
    dim "Platform: $platform"

    # Get latest version
    local tag
    tag="$(get_latest_version)"

    if [ -z "$tag" ]; then
        # No release yet — try building from source
        echo ""
        echo "No release found. Attempting build from source..."

        if ! command -v cc &>/dev/null && ! command -v gcc &>/dev/null; then
            error "No C compiler found. Install gcc or clang, or wait for a release."
        fi

        local tmpdir
        tmpdir="$(mktemp -d)"
        trap "rm -rf $tmpdir" EXIT

        echo "Cloning repository..."
        git clone --depth 1 "https://github.com/$REPO.git" "$tmpdir/swarmrt" 2>/dev/null

        echo "Building..."
        cd "$tmpdir/swarmrt"
        make mcp 2>/dev/null

        mkdir -p "$INSTALL_DIR"
        cp bin/swarmrt-mcp "$INSTALL_DIR/$BINARY_NAME"
        chmod +x "$INSTALL_DIR/$BINARY_NAME"

        info "Built from source and installed to $INSTALL_DIR/$BINARY_NAME"
    else
        local version="${tag#mcp-v}"
        dim "Version: $version ($tag)"

        local url="https://github.com/$REPO/releases/download/$tag/$BINARY_NAME-$platform"
        dim "URL: $url"

        # Download
        mkdir -p "$INSTALL_DIR"
        local dest="$INSTALL_DIR/$BINARY_NAME"

        echo ""
        echo "Downloading..."
        download "$url" "$dest"
        chmod +x "$dest"

        local size
        size=$(wc -c < "$dest" | tr -d ' ')
        info "Downloaded: $dest ($size bytes)"
    fi

    # Verify
    if [ ! -x "$INSTALL_DIR/$BINARY_NAME" ]; then
        error "Binary not found at $INSTALL_DIR/$BINARY_NAME"
    fi

    # Add to Claude Code
    echo ""
    if command -v claude &>/dev/null; then
        echo "Adding to Claude Code..."
        claude mcp add -s user swarmrt-mcp -- "$INSTALL_DIR/$BINARY_NAME" 2>/dev/null && {
            info "Added to Claude Code."
        } || {
            echo "Could not auto-add. Add manually:"
            echo "  claude mcp add -s user swarmrt-mcp -- $INSTALL_DIR/$BINARY_NAME"
        }
    else
        echo "Claude Code not found in PATH. Add manually:"
        echo "  claude mcp add -s user swarmrt-mcp -- $INSTALL_DIR/$BINARY_NAME"
    fi

    # Check PATH
    if [[ ":$PATH:" != *":$INSTALL_DIR:"* ]]; then
        echo ""
        dim "Note: $INSTALL_DIR is not in your PATH."
        dim "Add to your shell profile:"
        dim "  export PATH=\"$INSTALL_DIR:\$PATH\""
    fi

    echo ""
    info "Done. Restart Claude Code to activate 22 tools."
    echo ""
    echo "Tools: codebase_search, codebase_fuzzy, codebase_grep, codebase_overview,"
    echo "       git_diff, git_log, memory_store, memory_recall, memory_update,"
    echo "       autopilot_start, autopilot_status, autopilot_step, and more."
    echo ""
}

main "$@"
