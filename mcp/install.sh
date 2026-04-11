#!/bin/bash
# SwarmRT MCP — Install for Claude Code
# Usage: ./mcp/install.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BINARY="$ROOT_DIR/bin/swarmrt-mcp"
WRAP_BINARY="$ROOT_DIR/bin/swarmrt-wrap"
HOOK="$SCRIPT_DIR/autopilot_hook.sh"
SETTINGS_FILE="$HOME/.claude/settings.json"

echo "SwarmRT MCP Installer"
echo "====================="

# Build if needed
if [ ! -f "$BINARY" ]; then
    echo "Building swarmrt-mcp..."
    cd "$ROOT_DIR"
    make mcp
fi
if [ ! -f "$WRAP_BINARY" ]; then
    echo "Building swarmrt-wrap (PTY daemon for wake_* tools)..."
    cd "$ROOT_DIR"
    make mcp-wrap
fi

# Verify binaries
if [ ! -x "$BINARY" ]; then
    echo "Error: $BINARY not found or not executable"
    exit 1
fi

echo "MCP binary:  $BINARY ($(wc -c < "$BINARY" | tr -d ' ') bytes)"
if [ -x "$WRAP_BINARY" ]; then
    echo "Wrap binary: $WRAP_BINARY ($(wc -c < "$WRAP_BINARY" | tr -d ' ') bytes)"
fi

# Ensure .claude dir exists
mkdir -p "$(dirname "$SETTINGS_FILE")"

# Use python3 to merge into settings.json (handles all cases cleanly)
python3 << PYEOF
import json, os

settings_path = "$SETTINGS_FILE"
binary = "$BINARY"
hook = "$HOOK"

# Load or create
if os.path.exists(settings_path):
    with open(settings_path) as f:
        settings = json.load(f)
else:
    settings = {}

# Add MCP server
settings.setdefault("mcpServers", {})
settings["mcpServers"]["swarmrt-mcp"] = {
    "command": binary,
    "args": []
}

# Add autopilot hook (UserPromptSubmit — runs when Claude is about to respond)
settings.setdefault("hooks", {})
settings["hooks"].setdefault("UserPromptSubmit", [])

# Remove any existing swarmrt hooks
settings["hooks"]["UserPromptSubmit"] = [
    h for h in settings["hooks"]["UserPromptSubmit"]
    if "swarmrt" not in h.get("command", "")
]

# Add the autopilot hook
settings["hooks"]["UserPromptSubmit"].append({
    "command": hook,
    "timeout": 5000
})

with open(settings_path, "w") as f:
    json.dump(settings, f, indent=2)

print(f"MCP server: {binary}")
print(f"Autopilot hook: {hook}")
PYEOF

echo ""
echo "Installed. Restart Claude Code to activate."
echo ""
echo "Tools (33):"
echo "  Search:     codebase_search, codebase_fuzzy, codebase_grep, codebase_status, codebase_reindex"
echo "  Git:        git_diff, git_log"
echo "  Arch:       codebase_overview"
echo "  Memory:     memory_store, memory_update, memory_recall, memory_list, memory_forget"
echo "  Session:    session_log, session_context"
echo "  Autopilot:  autopilot_start, autopilot_status, autopilot_step, autopilot_pause, autopilot_stop"
echo "  Workspace:  workspace_create, workspace_list, workspace_archive, checkpoint_save, checkpoint_restore, workspace_diff"
echo "  Wake:       wake_create, wake_list, wake_delete, wake_enable, wake_fire_now"
echo "  System:     set_project, process_stats"
echo ""
echo "For wake_* tools to actually fire, launch Claude Code via the PTY wrapper:"
echo "  alias claude='$WRAP_BINARY claude'"
echo ""
echo "Without the wrapper, wake_create still saves to .swarmrt/wakes.json but nothing"
echo "injects prompts into the session. The wrapper polls wakes.json every 5s and"
echo "writes due prompts to the PTY as if you had typed them."
