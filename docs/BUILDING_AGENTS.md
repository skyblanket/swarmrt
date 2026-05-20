# Building AI Agents with SwarmRT

This is the developer-facing guide. *You* are writing sw to build an AI agent — a process that takes goals, talks to an LLM, runs tools, manages state, maybe coordinates with other agents. If you're going the other way (you're an LLM writing sw code on demand), see [AGENT_SYSTEM.md](AGENT_SYSTEM.md).

The short version: **swarmrt is what Erlang would look like if it was designed for AI agents in 2026 instead of telephone switches in 1986**. Same primitives — process, mailbox, supervisor, distribution — tuned for the actual workload: LLM streaming, JSON, HTTP, browser automation, structured tool dispatch.

If you've ever built an agent in Python with threading + asyncio + a tool-call parser + a retry layer + a mailbox abstraction stitched on top of `queue.Queue`, you've reinvented bad versions of what sw gives you in one binary.

---

## The model

| sw concept | What it is for an agent |
|---|---|
| **process** | an agent (or a tool worker, or a supervisor — same primitive) |
| **mailbox** | the agent's inbox; arrives async, drains in order or by selective match |
| **`receive`** | the agent's main wait point — block until a message that matches |
| **`spawn(fn(args))`** | give birth to a new agent in its own scheduler slot |
| **`send(pid, msg)`** | give an agent some work, ask for a reply, broadcast an update |
| **`register('name', pid)`** | name an agent so other agents can find it without knowing pids |
| **`ets_new()`** | a shared mutable table — perms cache, conversation memory, agent registry |
| **`supervise(strategy, children)`** | crash-recovery boundary; restart strategy decides who else dies when one child does |

You don't need anything else conceptually. Everything below is patterns built from these.

---

## The agent loop skeleton

Every long-lived agent has the same shape:

```sw
fun agent(name, state) {
    receive {
        {'task', payload, reply_to} ->
            new_state = handle_task(state, payload)
            send(reply_to, {'reply', name, result_of(new_state)})
            agent(name, new_state)

        {'status', from} ->
            send(from, {'status', name, summary(state)})
            agent(name, state)

        'stop' ->
            cleanup(state)
            'done'

        _other ->
            # Unknown messages — log and continue.
            print(f"[{name}] unknown: {_other}")
            agent(name, state)
    }
}

fun start(name, initial) {
    pid = spawn(agent(name, initial))
    register(name, pid)
    pid
}
```

**Three things to internalise:**

1. **The function is the agent.** No class, no struct, no `Agent` superclass. The function definition + the recursion is the entire agent definition.
2. **State is the recursion arg.** `agent(name, new_state)` is how you "update" state. The compiler turns self-tail-calls into `goto`, so the stack doesn't grow — the agent can run forever.
3. **The mailbox + selective receive are the dispatch system.** No event loop framework. No callback registration. `receive { pattern -> body }` waits until something matching shows up.

---

## Calling an LLM

For a quick non-streaming call:

```sw
fun ask_llm(prompt) {
    body = json_encode(%{
        model: "kimi-k2.6",
        messages: [%{role: "user", content: prompt}],
        max_tokens: 1024
    })
    hdrs = [{"Authorization", "Bearer " ++ getenv("API_KEY")},
            {"Content-Type", "application/json"}]
    resp = http_post("https://api.moonshot.ai/v1/chat/completions", hdrs, body)
    if (resp == nil) { nil }
    else {
        decoded = json_decode(resp)
        map_get(map_get(hd(map_get(decoded, 'choices')), 'message'), 'content')
    }
}
```

For real interactive usage you want streaming so the user sees tokens as they arrive:

```sw
# Same shape, but http_post_stream prints tokens to stdout as they
# arrive, with a Claude-Code-style spinner during dead air, and
# returns the full response wrapped in OpenAI-shape JSON.
resp = http_post_stream(url, hdrs, body_with_stream_true)
```

`http_post_stream` also:
- Detects reasoning-channel models (DeepSeek-R1, GLM-5.1, o1) and renders thinking dim+italic.
- Accepts ESC / Ctrl-C as a soft interrupt — kills the curl, returns the partial response with a marker the model sees next turn.
- Knows about token-limit truncation and surface it explicitly.

### Multi-agent streaming without TTY interleave

When you have many subagents calling LLMs in parallel, you don't want their token streams blasting onto stdout interleaved. Pass an extra `(target_pid, name)` pair and each chunk arrives as a message:

```sw
http_post_stream(url, hdrs, body, parent_pid, "scout-1")
# parent receives: {'stream_chunk', "scout-1", "hello"}, {'stream_chunk', "scout-1", " world"}, …, {'stream_done', "scout-1"}
```

The parent then renders chunks with a per-agent prefix and color, so 3 parallel subagents stay legible.

This is exactly the mechanism `swarm-code` uses under the hood — see `examples/dispatcher.sw` for the studio-pattern skeleton.

---

## Tool calls — parse + dispatch

The hard part of an agent is the tool-call protocol. There are three conventions worth knowing:

1. **Inband tags** (Gemma-style, swarm-code default): model emits `<tool name="bash">{"cmd":"ls"}</tool>` in its prose. You scan the response for the tag.
2. **OpenAI-native** (Kimi, GPT-4-tools, Claude): structured `tool_calls` array in the JSON response. Parse the field; no string scanning.
3. **Markdown JSON** (some open-weights): model emits ` ```json {"tool":"bash","args":{…}}``` `. Strip the fence, decode.

For inband (the simplest), the parser is ~20 lines and dispatch is one `case`:

```sw
fun parse_tools(text) { parse_loop(text, []) }

fun parse_loop(text, acc) {
    i = string_index_of(text, "<tool name=\"")
    if (i < 0) { acc }
    else {
        rest = string_sub(text, i + 12, string_length(text) - (i + 12))
        name_end = string_index_of(rest, "\"")
        name = string_sub(rest, 0, name_end)
        after = string_sub(rest, name_end + 2, string_length(rest) - (name_end + 2))
        close = string_index_of(after, "</tool>")
        args = json_decode(string_sub(after, 0, close))
        tail = string_sub(after, close + 7, string_length(after) - (close + 7))
        parse_loop(tail, list_append(acc, {name, args}))
    }
}

fun run_tool(name, args) {
    case name {
        "bash"   -> elem(shell(map_get(args, 'cmd')), 1)
        "read"   -> file_read(map_get(args, 'path'))
        "fetch"  -> http_get(map_get(args, 'url'), [])
        "search" -> web_search_impl(map_get(args, 'q'))
        _        -> f"error: unknown tool '{name}'"
    }
}
```

`case` is exactly the right shape for tool dispatch — flat, pattern-matched, falls through to a catchall. The full loop is in [`examples/llm_agent.sw`](../examples/llm_agent.sw) — a complete agent in ~90 lines.

---

## The studio pattern — many agents, one orchestrator

When you want a planning agent that spawns specialists ("research this," "fact-check that"), use the studio pattern:

```sw
# Spawn — returns the worker pid, registered by name.
fun spawn_specialist(name, role, goal) {
    pid = spawn(specialist_loop(name, role, goal, []))
    register(name, pid)
    pid
}

# Ask — block until this specific specialist replies (selective receive
# means other agents' replies stay queued for later).
fun ask(name, prompt) {
    send(whereis(name), {'task', prompt, self()})
    receive { {'reply', n, content} when n == name -> content }
}

# Parallel — spawn N, send N tasks, collect N replies.
fun parallel_ask(tasks) {
    names = spawn_each(tasks, [])
    send_each(tasks)
    collect(names, [])
}
```

The full implementation (parallel collection, lifecycle management, stream multiplexing) is in `swarm-code/src/agents.sw` if you want a battle-tested reference.

**Selective receive is the superpower here.** When the orchestrator does `ask("scout-1", "…")`, it blocks on `{'reply', "scout-1", _}` *specifically*. All the other agents' replies, status updates, and stream chunks stay in its mailbox for the next `receive` call. No callback registration, no future/promise plumbing, no event-loop framework.

---

## Shared state with ETS

Maps and lists are immutable — when an agent updates state, it recurses with a new map. That's fine for the agent's own state, but you often need state shared between agents: a permissions cache, conversation memory across sessions, a registry of which agents are alive.

ETS gives you a concurrent in-memory table:

```sw
# Create once at startup, pass the handle to everyone.
perms = ets_new()
ets_put(perms, 'bash-allowed', 'true')

# In any agent:
allowed = ets_get(perms, 'bash-allowed')
if (allowed == 'true') { run_bash_tool() } else { ask_user_to_allow() }
```

Pass the table value into spawned processes — they share backing storage. `ets_list(t)` enumerates entries; `ets_count(t)` is the size.

---

## Supervised crash recovery

For agents that should restart on crash (a long-running watcher, a stateful tool worker), wrap them in a supervisor:

```sw
sup = supervise('one_for_one', [
    {'watcher_a', fun() { watcher("A", []) }, 'permanent'},
    {'watcher_b', fun() { watcher("B", []) }, 'permanent'},
    {'tool_pool', fun() { tool_pool(5) }, 'transient'}
])
```

Strategies:

| Strategy | Behaviour |
|---|---|
| **`one_for_one`** | one child dies → only that child restarts |
| **`one_for_all`** | one child dies → kill all siblings, restart all |
| **`rest_for_one`** | one child dies → kill siblings spawned after it, restart them |

Restart types: `'permanent'` (always restart), `'transient'` (restart only on abnormal exit), `'temporary'` (never restart).

This is the Erlang "let it crash" philosophy. An agent process that hits a panic or an `error()` raise will die; the supervisor catches it and starts a fresh one. State that was in the dead agent is lost (recurse with fresh state on restart) — for state you want to survive crashes, put it in ETS or a file.

---

## Tools that need a browser

Use `chrome_launch` + the WebSocket client. No Playwright, no Node sidecar:

```sw
port = chrome_launch()    # finds Chromium, spawns headed Chrome with CDP
# Connect via CDP, navigate, screenshot — full pipeline in
# examples/http_echo.sw + swarm-code's browser.sw
```

The Chrome integration is the same one swarm-code uses for web tools — see `swarm-code/src/browser.sw` for the full CDP client (navigate, click, type, screenshot, get_text, get_html, evaluate JavaScript, close).

---

## MCP (Model Context Protocol)

You don't need to hand-roll JSON-RPC — `import Mcp` gives you both sides:

```sw
import Mcp

# Client: consume tools from any MCP server (stdio transport).
client = Mcp.client_start("npx -y @modelcontextprotocol/server-filesystem /tmp")
tools  = Mcp.list_tools(client)
result = Mcp.call_tool(client, "read_file", %{path: "/tmp/x"})
Mcp.close(client)

# Server: expose your own tools as MCP.
Mcp.serve(%{
    name: "my-agent-tools",
    version: "0.1",
    tools: [
        %{name: "echo", description: "echo back",
          input_schema: %{type: "object", properties: %{msg: %{type: "string"}}},
          handler: fun(args) { map_get(args, 'msg') }}
    ]
})
```

For other RPC protocols, the raw `wsc_connect / wsc_send / wsc_recv / wsc_close` builtins are still there.

---

## Memory: SQLite + embeddings + vector store

For structured state (conversation history, tasks, telemetry rollups) use SQLite:

```sw
db = db_open("./agent.db")
db_exec(db, "CREATE TABLE IF NOT EXISTS conv (id INTEGER PRIMARY KEY, turn TEXT, role TEXT)")
db_exec(db, f"INSERT INTO conv (role, turn) VALUES ('user', '{escape(user_input)}')")
rows = db_query(db, "SELECT role, turn FROM conv WHERE id > ?", [last_seen])
```

For semantic memory (retrieve-the-3-most-relevant-prior-conversations), pair `Embed` + `Vec`:

```sw
import Embed
import Vec

opts = %{endpoint: "https://api.openai.com", key: getenv("OPENAI_KEY"),
         model: "text-embedding-3-small"}
store = Vec.new()

# Index
vec = Embed.create(opts, "the quick brown fox")
Vec.add(store, "doc-1", vec, %{text: "the quick brown fox"})

# Query
qvec = Embed.create(opts, "speedy animal")
hits = Vec.search(store, qvec, 3)
# → [{id, score, payload}, ...] top-3 by cosine similarity
```

`Vec` is O(N) per query — fine to ~100K vectors. For bigger, swap in an external service via `wsc_*`.

---

## Autonomy: scheduled work

For agents that need to do something every N seconds / at a specific time:

```sw
import Cron

Cron.every(30000, fn() { check_inbox() })          # every 30 sec
Cron.at("09:00", fn() { send_daily_report() })     # daily at 09:00 local
Cron.in_ms(5000, fn() { print("one-shot timer") }) # one-shot in 5 sec
```

Each call returns a wake-pid you can send `'stop'` to (or use `Cron.stop(pid)`).

---

## Observability: telemetry

One emit point, pluggable sinks:

```sw
import Telemetry

Telemetry.configure(%{sinks: [
    Telemetry.stdout_sink(),
    Telemetry.jsonl_sink("/var/log/agent.jsonl")
]})

Telemetry.emit("tool.bash.start", %{cmd: "ls"})
# ... run tool ...
Telemetry.emit("tool.bash.end",   %{exit_code: 0, ms: 42})
```

Custom sinks are just functions `fun(event_map) { ... }`. Push to OpenTelemetry, Slack webhooks, a dashboard ETS table — your call.

---

## Prompt templates

Stop stuffing 200-line strings inline:

```sw
import Prompt

system = Prompt.from_file("prompts/system.md",
                          %{user: "Sky", role: "agent", date: today()})

# Or inline strings:
greeting = Prompt.render("Hello {{name}}, you have {{n}} messages",
                         %{name: "Alice", n: 3})
```

Missing variables render empty by default; `Prompt.render_strict` panics on missing instead.

---

## Putting it together

A complete LLM-driven agent that takes a question, calls an LLM, parses tool tags, dispatches them, loops:

→ **[`examples/llm_agent.sw`](../examples/llm_agent.sw)** (~90 lines, real working code)

A multi-agent dispatcher with tagged-message routing:

→ **[`examples/dispatcher.sw`](../examples/dispatcher.sw)** (~50 lines)

A working HTTP server agents can expose as a tool:

→ **[`examples/http_echo.sw`](../examples/http_echo.sw)** (~25 lines)

A real-world agent stack that uses all of this in anger:

→ **[swarm-code](https://github.com/skyblanket/swarm-code)** — a terminal coding agent (private repo, ask Sky for access)

---

## When the runtime feels wrong, it's usually because

- **You forgot a base case in your tail-recursive loop.** Stack doesn't grow, but the agent will spin if the recursion's terminating condition is never met. Always `case state { 'done' -> 'bye' ; _ -> agent(new_state) }` or similar.
- **You're holding state in a global instead of recursing.** sw has no globals. Use the recursion arg, or ETS if it's actually shared. The temptation to reach for a global usually means the design is muddled.
- **You're using `if/else` for what should be `case`.** If you find yourself with `else { if (x) { … } else { if (y) { … } } }`, that's a `case x { … }` waiting to be written.
- **Your agent's `receive` clauses don't have a catchall.** Unmatched messages stay queued forever — your agent will start growing memory invisibly. Either match exhaustively or end with `_other -> agent(state)`.
- **You're trying to make a synchronous HTTP call from inside a `receive` arm and it feels off.** It's fine — the process suspends on the syscall, the scheduler runs other processes meanwhile. That's what the runtime is for.

---

## See also

- [SW_LANGUAGE.md](SW_LANGUAGE.md) — full language reference
- [AGENT_SYSTEM.md](AGENT_SYSTEM.md) — cheatsheet for an LLM writing sw code (different audience: agents writing sw, not developers writing agents)
- [ARCHITECTURE.md](ARCHITECTURE.md) — runtime internals (schedulers, mailboxes, GC, distribution)
- [BENCHMARKS.md](BENCHMARKS.md) — process spawn, context switch, message send numbers
