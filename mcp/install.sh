#!/bin/bash
# SwarmRT MCP — Install for Claude Code
# Usage: ./mcp/install.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BINARY="$ROOT_DIR/bin/swarmrt-mcp"
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

# Verify binary
if [ ! -x "$BINARY" ]; then
    echo "Error: $BINARY not found or not executable"
    exit 1
fi

echo "Binary: $BINARY ($(wc -c < "$BINARY" | tr -d ' ') bytes)"

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
echo "Tools (15):"
echo "  Search:    codebase_search, codebase_fuzzy, codebase_status, codebase_reindex"
echo "  Memory:    memory_store, memory_recall, memory_list, memory_forget"
echo "  Session:   session_log, session_context"
echo "  Autopilot: autopilot_start, autopilot_status, autopilot_step, autopilot_stop"
echo "  Stats:     process_stats"
