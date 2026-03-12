# SwarmRT MCP Server

The last MCP server you'll need for Claude Code. One native binary, 22 tools, zero dependencies.

Replaces SuperMemory, Claude Context, Ralph Wiggum, Codebase Memory MCP, and Desktop Commander — all in 108KB.

## Quick Install

```bash
curl -fsSL https://raw.githubusercontent.com/skyblanket/swarmrt/main/mcp/get.sh | bash
```

Or manually:

```bash
# macOS (Apple Silicon)
curl -fsSL -o swarmrt-mcp https://github.com/skyblanket/swarmrt/releases/latest/download/swarmrt-mcp-darwin-arm64
chmod +x swarmrt-mcp

# macOS (Intel)
curl -fsSL -o swarmrt-mcp https://github.com/skyblanket/swarmrt/releases/latest/download/swarmrt-mcp-darwin-x86_64
chmod +x swarmrt-mcp

# Linux (x86_64)
curl -fsSL -o swarmrt-mcp https://github.com/skyblanket/swarmrt/releases/latest/download/swarmrt-mcp-linux-x86_64
chmod +x swarmrt-mcp

# Linux (ARM64)
curl -fsSL -o swarmrt-mcp https://github.com/skyblanket/swarmrt/releases/latest/download/swarmrt-mcp-linux-arm64
chmod +x swarmrt-mcp
```

Then add to Claude Code:

```bash
claude mcp add -s user swarmrt-mcp -- /path/to/swarmrt-mcp
```

Or add to `~/.claude.json` manually:

```json
{
  "mcpServers": {
    "swarmrt-mcp": {
      "command": "/path/to/swarmrt-mcp",
      "args": []
    }
  }
}
```

Restart Claude Code. Done.

## Build from Source

Requires: C compiler (gcc or clang), make, zlib

```bash
git clone https://github.com/skyblanket/swarmrt.git
cd swarmrt
make mcp
# Binary at bin/swarmrt-mcp
```

## Tools (22)

### Search

| Tool | Description |
|------|-------------|
| `codebase_search` | BM25 keyword search over the indexed codebase. Returns ranked matches with file:line, relevance scores, and content snippets. |
| `codebase_fuzzy` | Typo-tolerant trigram search. Finds matches even with misspelled queries. |
| `codebase_grep` | POSIX extended regex search over actual files. Returns file:line:text for exact pattern matching. |
| `codebase_status` | Index stats: file count, token count, trigram count, memory usage. |
| `codebase_reindex` | Force a full re-index of the codebase. |

### Git

| Tool | Description |
|------|-------------|
| `git_diff` | Show changed files with stats (insertions/deletions) and truncated patch. Optionally diff against a specific ref (branch, commit, `HEAD~N`). |
| `git_log` | Recent commits as structured JSON (hash, author, date, message). Filter by file path. |

### Architecture

| Tool | Description |
|------|-------------|
| `codebase_overview` | Structural overview: auto-detects 45+ languages, file/line counts per language, directory structure (2 levels), git HEAD. |

### Memory (persistent across sessions)

| Tool | Description |
|------|-------------|
| `memory_store` | Store a key/value memory with optional tags. Persists to `.swarmrt/memories.jsonl`. |
| `memory_update` | Append to an existing memory's value (or create if new). Merges tags. |
| `memory_recall` | BM25 search over stored memories. Returns ranked results by relevance. |
| `memory_list` | List all memories, optionally filtered by tag. |
| `memory_forget` | Delete a memory by key. |

### Session

| Tool | Description |
|------|-------------|
| `session_log` | Log a typed event (note, decision, error, discovery) to session history. |
| `session_context` | BM25 search over session history. Find relevant past events. |

### Autopilot (autonomous mode)

| Tool | Description |
|------|-------------|
| `autopilot_start` | Start autonomous mode with a goal and ordered steps. Claude keeps working until all steps are complete. |
| `autopilot_status` | Check current goal, step progress, elapsed time, iteration count. |
| `autopilot_step` | Mark current step done and advance. Include a summary of what was accomplished. |
| `autopilot_pause` | Toggle pause/resume. When paused, the hook won't re-feed Claude. |
| `autopilot_stop` | Stop autonomous mode (completed, aborted, or blocked). |

### System

| Tool | Description |
|------|-------------|
| `set_project` | Switch to a different project directory. Resets search index, reloads memories and autopilot state. |
| `process_stats` | Runtime statistics: version, codebase index size, memory count, session events. |

## How It Works

- **Search engine**: SwarmRT's native BM25 + trigram fuzzy search with SIMD acceleration. Indexes on first search call (lazy), persists index to `.swarmrt/index.sws`.
- **Memory**: JSONL file at `.swarmrt/memories.jsonl`, loaded into a BM25 search index for recall.
- **Autopilot**: State persisted to `.swarmrt/autopilot.json`. Optional hook script re-feeds Claude when it tries to stop.
- **Protocol**: MCP (JSON-RPC 2.0 over stdio, newline-delimited). Compatible with Claude Code, Cursor, and any MCP client.

## Autopilot Hook (optional)

The autopilot hook makes Claude keep working autonomously. To enable:

1. Add to `~/.claude/settings.json`:

```json
{
  "hooks": {
    "UserPromptSubmit": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "/path/to/swarmrt/mcp/autopilot_hook.sh",
            "timeout": 5000
          }
        ]
      }
    ]
  }
}
```

2. Start autopilot via the `autopilot_start` tool with a goal and steps.
3. Claude will keep working through each step, calling `autopilot_step` when done.
4. Use `autopilot_pause` to temporarily stop, `autopilot_stop` to end.

## Data Storage

All data is stored in `{project_root}/.swarmrt/`:

```
.swarmrt/
  index.sws          # Persisted search index
  memories.jsonl      # Key/value memories
  autopilot.json      # Autopilot state
```

Add `.swarmrt/` to your `.gitignore`.

## Performance

| Metric | Value |
|--------|-------|
| Binary size | 108 KB |
| Startup time | < 1 ms (lazy indexing) |
| Index 183 files | 54 ms |
| BM25 search | < 1 ms |
| Memory usage | ~3 MB for 200-file project |

## Comparison

| Feature | SwarmRT MCP | SuperMemory | Claude Context | Codebase Memory MCP |
|---------|:-----------:|:-----------:|:--------------:|:-------------------:|
| Binary size | 108 KB | ~100 MB (npm) | ~80 MB (npm) | ~15 MB (Go) |
| Dependencies | 0 | Node.js | Node.js + Python | Go + CGO |
| BM25 search | yes | no | yes | no |
| Fuzzy search | yes | no | yes | no |
| Regex grep | yes | no | no | yes |
| Persistent memory | yes | yes | no | no |
| Git integration | yes | no | no | yes |
| Autopilot mode | yes | no | no | no |
| Language detection | 45+ | no | no | 64 (tree-sitter) |
| Session context | yes | no | no | no |

## Supported Platforms

| Platform | Architecture | Status |
|----------|-------------|--------|
| macOS | Apple Silicon (arm64) | Primary |
| macOS | Intel (x86_64) | Supported |
| Linux | x86_64 | Supported |
| Linux | ARM64 (aarch64) | Supported |

## License

Part of SwarmRT. MIT License.
