# SwarmRT MCP Server

The last MCP server you'll need for Claude Code. One native binary, 36 tools, zero dependencies.

Replaces SuperMemory, Claude Context, Ralph Wiggum, Codebase Memory MCP, and Desktop Commander — all in ~128KB. Ships with an optional PTY wrapper (`swarmrt-wrap`) that gives Claude Code true time-triggered autonomy via the `wake_*` tools — ungated, local, resume-safe, no cloud — and three `feedback_*` tools that ship structured agent observations to a shared public server at **https://swarmrt-feedback.fly.dev** so the tool can be improved by evidence rather than guesswork.

## One-shot install (for LLM agents)

If you're an LLM installing this on a user's machine, run these exact commands in order. All are idempotent and safe to re-run:

```bash
# 1. Clone (skip if already cloned)
test -d ~/swarmrt || git clone https://github.com/skyblanket/swarmrt.git ~/swarmrt

# 2. Build both binaries (requires: cc, make, zlib)
cd ~/swarmrt && make mcp mcp-wrap

# 3. Verify the binaries exist and are executable
test -x ~/swarmrt/bin/swarmrt-mcp  && echo "mcp OK"
test -x ~/swarmrt/bin/swarmrt-wrap && echo "wrap OK"

# 4. Register the MCP server with Claude Code (user scope)
claude mcp add -s user swarmrt-mcp -- ~/swarmrt/bin/swarmrt-mcp

# 5. Add the PTY wrapper alias to the user's shell rc.
#    Detect their shell and append to the right file.
RC="$HOME/.zshrc"
[ -n "$BASH_VERSION" ] && RC="$HOME/.bashrc"
grep -q "swarmrt-wrap claude" "$RC" 2>/dev/null || \
  echo "alias claude='$HOME/swarmrt/bin/swarmrt-wrap claude'" >> "$RC"

# 6. Tell the user to either `source $RC` or restart their shell, then
#    launch Claude Code with `claude` (which will now go through the wrapper).
```

**Post-install verification**: In a fresh Claude Code session, ask Claude to call `wake_list` — it should return `{"wakes":[],"count":0,"now":...}`. That proves the MCP is wired up. To verify the wrapper, run:

```bash
# This should build, run for 10s, and print "AUTONOMY-OK" once from a cron fire.
cd /tmp && mkdir -p wake_verify && cd wake_verify && rm -rf .swarmrt
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"v","version":"0"}}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"wake_create","arguments":{"cron_expression":"* * * * *","prompt":"AUTONOMY-OK"}}}' \
  | ~/swarmrt/bin/swarmrt-mcp /tmp/wake_verify >/dev/null 2>&1
(sleep 70; echo) | ~/swarmrt/bin/swarmrt-wrap bash -c 'stty -echo 2>/dev/null; cat'
```

You should see `AUTONOMY-OK` appear on its own at the next minute boundary within the 70-second window. If not, check `.swarmrt/wakes.json` exists and `bin/swarmrt-wrap` is executable.

### Gotchas for LLM installers

- **The alias is mandatory for wake delivery.** `wake_create` / `wake_list` / etc. work without the wrapper, but nothing ever fires. The wrapper is the timer engine; the MCP is the configuration store.
- **One `.swarmrt/` per project.** State lives in `{cwd}/.swarmrt/`, not `$HOME`. Each project directory gets its own independent wake schedule. New sessions and `claude --resume` in the same project inherit the schedule automatically.
- **Cron is in local time.** `0 9 * * 1-5` means 9am in `$TZ`, not UTC. Confirm the user's timezone if they give you a cron that's ambiguous.
- **Minimum granularity is 1 minute.** Sub-minute scheduling isn't possible with 5-field cron — use `wake_fire_now` for immediate testing instead of inventing a `*/10 seconds` syntax.
- **Vixie OR for dom+dow.** If BOTH day-of-month and day-of-week are restricted (neither is `*`), the wake fires when *either* matches. Most humans expect AND — if the user wants "only on the 15th if it's also a weekday," tell them it's easier to handle that in the prompt than in cron.
- **Don't touch `~/.claude/settings.json` by hand.** Use `claude mcp add` (step 4 above). Manual edits can silently break hook config.
- **Feedback is opt-in and off by default.** `feedback_report` silently declines unless `SWARMRT_FEEDBACK_ENABLED=1`. If you want your agent to actively contribute observations to the shared pool, have the user add `export SWARMRT_FEEDBACK_ENABLED=1` to their shell rc. `feedback_read` and `feedback_stats` are always available (they're read-only public GETs). Defaults to `https://swarmrt-feedback.fly.dev`; override with `SWARMRT_FEEDBACK_URL` if self-hosting.
- **Feedback server rejects paths in free-text.** If you try to `feedback_report` a message containing `/Users/...` or `/home/...`, the server returns HTTP 400 with a privacy error. Summarize in plain English. Reference file locations as e.g. `"the MCP's tool_wake_list in swarmrt_mcp.c"`, never `"/Users/sky/swarmrt/mcp/swarmrt_mcp.c:1373"`.

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

## Tools (28)

### Search (5)

| Tool | Description |
|------|-------------|
| `codebase_search` | BM25 keyword search over the indexed codebase. Returns ranked matches with file:line, relevance scores, and content snippets. |
| `codebase_fuzzy` | Typo-tolerant trigram search. Finds matches even with misspelled queries. |
| `codebase_grep` | POSIX extended regex search over actual files. Returns file:line:text for exact pattern matching. |
| `codebase_status` | Index stats: file count, token count, trigram count, memory usage. |
| `codebase_reindex` | Force a full re-index of the codebase. |

### Git (2)

| Tool | Description |
|------|-------------|
| `git_diff` | Show changed files with stats (insertions/deletions) and truncated patch. Optionally diff against a specific ref (branch, commit, `HEAD~N`). |
| `git_log` | Recent commits as structured JSON (hash, author, date, message). Filter by file path. |

### Architecture (1)

| Tool | Description |
|------|-------------|
| `codebase_overview` | Structural overview: auto-detects 45+ languages, file/line counts per language, directory structure (2 levels), git HEAD. |

### Memory (5, persistent across sessions)

| Tool | Description |
|------|-------------|
| `memory_store` | Store a key/value memory with optional tags. Persists to `.swarmrt/memories.jsonl`. |
| `memory_update` | Append to an existing memory's value (or create if new). Merges tags. |
| `memory_recall` | BM25 search over stored memories. Returns ranked results by relevance. |
| `memory_list` | List all memories, optionally filtered by tag. |
| `memory_forget` | Delete a memory by key. |

### Session (2)

| Tool | Description |
|------|-------------|
| `session_log` | Log a typed event (note, decision, error, discovery) to session history. |
| `session_context` | BM25 search over session history. Find relevant past events. |

### Autopilot (5, autonomous mode)

| Tool | Description |
|------|-------------|
| `autopilot_start` | Start autonomous mode with a goal and ordered steps. Claude keeps working until all steps are complete. |
| `autopilot_status` | Check current goal, step progress, elapsed time, iteration count. |
| `autopilot_step` | Mark current step done and advance. Include a summary of what was accomplished. |
| `autopilot_pause` | Toggle pause/resume. When paused, the hook won't re-feed Claude. |
| `autopilot_stop` | Stop autonomous mode (completed, aborted, or blocked). |

### Workspaces (6, git worktree orchestration)

| Tool | Description |
|------|-------------|
| `workspace_create` | Create an isolated workspace (git worktree) for parallel development. Auto-assigns a city name if none provided. |
| `workspace_list` | List all active workspaces with branches, paths, and change status. |
| `workspace_archive` | Archive (delete) a workspace — removes the git worktree and branch. |
| `checkpoint_save` | Save a checkpoint (snapshot) of a workspace's current state. Commits all changes as a restorable reference. |
| `checkpoint_restore` | Restore a workspace to a previous checkpoint. Without a ref, lists available checkpoints. |
| `workspace_diff` | Show all changes in a workspace compared to the base branch. File stats, commit count, and diff content. |

### Feedback (3, shared server, opt-in)

| Tool | Description |
|------|-------------|
| `feedback_report` | Ship a structured observation to the shared feedback server. Opt-in: does nothing unless `SWARMRT_FEEDBACK_ENABLED=1`. Categories: `bug`, `confusion`, `wish`, `works-well`. Severities: `low`, `med`, `high`. Anonymous (`machine_id` = fnv1a hash of hostname). Server rejects free-text containing filesystem paths for privacy. |
| `feedback_read` | Public read: query the shared feedback pool. Filter by `since`, `category`, `tool`, `severity_min`. Use for triage — see what other agents reported. |
| `feedback_stats` | Aggregate counts per category + top-10 most-reported tools. Public. |

See [feedback-server/README.md](../feedback-server/README.md) for the server-side schema, deployment, and self-hosting instructions.

### Wake (5, time-triggered prompt injection)

| Tool | Description |
|------|-------------|
| `wake_create` | Schedule a prompt to be injected into the current Claude Code session on a 5-field cron schedule (local timezone). Persists to `.swarmrt/wakes.json` — survives session restarts and `--resume`. Examples: `*/15 * * * *`, `0 */3 * * *`, `0 9,13,17 * * 1-5`. Minimum granularity is 1 minute. Requires `swarmrt-wrap` to be the process wrapping Claude Code for prompts to actually fire. |
| `wake_list` | List all scheduled wakes with their cron, prompt, enabled state, next fire time, last scheduled fire time, scheduled fire count, last manual fire time, and manual fire count. Scheduled and manual counters are tracked separately — `fire_count` / `last_fired_at` reflect cron deliveries only, while `manual_fire_count` / `last_manual_fire_at` reflect `wake_fire_now` deliveries. This keeps the scheduled cadence observable without being polluted by debug pokes. |
| `wake_delete` | Delete a scheduled wake by id or name. |
| `wake_enable` | Enable or disable a wake without deleting it. |
| `wake_fire_now` | Manually queue a wake to fire on the wrapper's next 5-second tick — useful for testing a scheduled prompt without waiting for its cron slot. |

### System (2)

| Tool | Description |
|------|-------------|
| `set_project` | Switch to a different project directory. Resets search index, reloads memories, autopilot state, and wake schedule. |
| `process_stats` | Runtime statistics: version, codebase index size, memory count, session events. |

## How It Works

- **Search engine**: SwarmRT's native BM25 + trigram fuzzy search with SIMD acceleration. Indexes on first search call (lazy), persists index to `.swarmrt/index.sws`.
- **Memory**: JSONL file at `.swarmrt/memories.jsonl`, loaded into a BM25 search index for recall.
- **Autopilot**: State persisted to `.swarmrt/autopilot.json`. Optional hook script re-feeds Claude when it tries to stop.
- **Wake engine**: Cron schedule in `.swarmrt/wakes.json`; immediate-fire queue in `.swarmrt/wake_queue.jsonl`. The MCP is stateless between tool calls, so delivery is handled by a separate `swarmrt-wrap` binary that wraps Claude Code in a PTY, polls the schedule every 5 seconds, and injects due prompts by writing them to the PTY master — exactly as if the user had typed them and hit Enter.
- **Protocol**: MCP (JSON-RPC 2.0 over stdio, newline-delimited). Compatible with Claude Code, Cursor, and any MCP client.

## Wake Engine: truly autonomous Claude Code

Claude Code's built-in `/loop` (CronCreate) only fires while the REPL is mid-turn, and `/schedule` (remote triggers) needs a claude.ai OAuth account, a cloud environment, and a 1-hour minimum. The SwarmRT wake engine has none of those constraints:

- **Local only** — no cloud, no OAuth, no account gates
- **Minute granularity** — no 1-hour floor
- **Resume-safe** — state lives in `.swarmrt/wakes.json` at the project root, so any new session in that project (or `--resume` of an old one) picks up the schedule automatically
- **Ungated** — no GrowthBook feature flags; works on API-key accounts, Bedrock, Vertex, etc.

### Setup

1. Build the wrapper: `make mcp-wrap` (produces `bin/swarmrt-wrap`)
2. Alias Claude Code: `alias claude='/path/to/swarmrt-wrap claude'`
3. Inside any Claude Code session, ask: *"wake me every 15 minutes to check the deploy status"* — Claude calls `wake_create` and you're done.

### Example cron expressions

```
*/15 * * * *        every 15 minutes
0 */3 * * *         every 3 hours on the hour
0 9,13,17 * * 1-5   9am, 1pm, 5pm on weekdays
0 */2 9-17 * *      every 2 hours between 9am-5pm (any day)
0 10 * * 1,3,5      10am Mon/Wed/Fri
```

Cron uses the host's local timezone. Standard Vixie semantics apply (day-of-month OR day-of-week when both are restricted).

### Without the wrapper

If you run plain `claude` instead of `swarmrt-wrap claude`, all the `wake_*` tools still work for creating, listing, and deleting schedules — but nothing fires because nothing is polling the file and writing to the PTY. The wrapper is the delivery engine; the MCP is the configuration store.

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
  workspaces.json     # Active workspace registry
  wakes.json          # Wake engine cron schedule (shared with swarmrt-wrap)
  wake_queue.jsonl    # Immediate-fire queue drained by swarmrt-wrap
```

Add `.swarmrt/` to your `.gitignore`.

## Performance

| Metric | Value |
|--------|-------|
| Binary size | ~128 KB |
| Startup time | < 1 ms (lazy indexing) |
| Index 183 files | 54 ms |
| BM25 search | < 1 ms |
| Memory usage | ~3 MB for 200-file project |

## Comparison

| Feature | SwarmRT MCP | SuperMemory | Claude Context | Codebase Memory MCP |
|---------|:-----------:|:-----------:|:--------------:|:-------------------:|
| Binary size | ~128 KB | ~100 MB (npm) | ~80 MB (npm) | ~15 MB (Go) |
| Dependencies | 0 | Node.js | Node.js + Python | Go + CGO |
| BM25 search | yes | no | yes | no |
| Fuzzy search | yes | no | yes | no |
| Regex grep | yes | no | no | yes |
| Persistent memory | yes | yes | no | no |
| Git integration | yes | no | no | yes |
| Autopilot mode | yes | no | no | no |
| Git worktrees | yes | no | no | no |
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
