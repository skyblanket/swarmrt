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

`http_post`/`http_get` return body-or-nil and **hide the HTTP status** — fine for
"give me the body", but a REST client that has to tell a `200` from a `4xx`/`5xx`
that *also* carries a body (a Telnyx Call Control error envelope, say) can't. Use
`http_request(url, opts)` for that: it returns a status-aware map (or `{'error,
reason}` when the request never completed), without changing `http_post`/`http_get`:

```sw
# opts: method (default "GET"), headers (a %{k=>v} MAP or a list of {k,v}
# tuples), body. The response carries the status code + response headers.
fun dial(to) {
    body = json_encode(%{to: to, from: getenv("FROM")})
    opts = %{method: "POST",
             headers: %{"Authorization" => "Bearer " ++ getenv("TELNYX_KEY"),
                        "Content-Type"  => "application/json"},
             body: body}
    case http_request("https://api.telnyx.com/v2/calls", opts) {
        {'error', why} -> {'transport_error', why}          # never reached the API
        resp ->
            status = map_get(resp, "status")
            if (status >= 200 && status < 300) { {'ok', map_get(resp, "body")} }
            else { {'api_error', status, map_get(resp, "body")} }  # 4xx/5xx WITH body
    }
}
```

This is the gap the voice-agent port flagged: with only `http_post` it had to
*guess* success from the JSON envelope shape (`{"data":...}` == ok); `http_request`
lets it branch on the real status code instead.

For real interactive usage you want streaming so the user sees tokens as they arrive:

```sw
# Same shape, but http_post_stream prints tokens to stdout as they
# arrive, with a Claude-Code-style spinner during dead air, and
# returns a TAGGED result the caller branches on:
#   {'ok, json}    — success; json is OpenAI-shaped (choices[0].message.*)
#   {'error, why}  — curl died / non-2xx HTTP / empty-or-unparseable stream
case http_post_stream(url, hdrs, body_with_stream_true) {
    {'error', why} -> handle_failure(why)       # retry / surface / give up
    {'ok', json}   ->
        decoded = json_decode(json)
        map_get(map_get(hd(map_get(decoded, 'choices')), 'message'), 'content')
}
```

The tag is the whole point: a bare empty string used to mean *three different
things* — "the model said nothing", "the SSE stream failed to parse", and
"curl couldn't connect" — and an agent could not tell them apart to decide
whether to retry. Now `{'ok, json}` with empty content means the model
genuinely produced nothing, while `{'error, why}` means a real failure with an
actionable reason (`"curl exit 7: Connection refused"`, `"HTTP 404: {...}"`,
`"stream produced no content..."`).

**SSE shape the parser expects.** Each chunk must be a line beginning with
`data:` followed by an OpenAI-style JSON object. The space after the colon is
optional (`data: {...}` and `data:{...}` both parse). Content is read from
`choices[0].delta.content` (streaming) and accumulated across chunks; reasoning
from `choices[0].delta.reasoning_content`; the stream ends on `data: [DONE]`.
A server that emits any other framing will come back `{'error, "stream
produced no content..."}` rather than a silent empty string.

`http_post_stream` also:
- Detects reasoning-channel models (DeepSeek-R1, GLM-5.1, o1) and renders thinking dim+italic.
- Accepts ESC / Ctrl-C as a soft interrupt — kills the curl, returns `{'ok, partial}` with a marker the model sees next turn (an interrupt is not an error).
- Knows about token-limit truncation and surfaces it explicitly (still `{'ok, ...}` — the truncated content is real).

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

## Records: named state instead of positional tuples

Agent state is usually a few related fields — a call's id/from/state, a job's id/status/result. Tuples make this positional (`elem(job, 1)`), which is exactly the kind of off-by-one an LLM gets wrong. Use a record instead:

```sw
import Record

fun new_job(id, prompt) {
    Record.build('Job', ['id', 'prompt', 'status', 'result'],
                 %{id: id, prompt: prompt, status: 'queued', result: nil})
}

fun mark_done(job, result) {
    Record.update(Record.update(job, 'status', 'done'), 'result', result)
}
```

Records are tagged maps, so they send over messages, survive `json_encode`, and match in `receive` / `case` with normal `%{...}` patterns. `Record.build` checks every field is present when you construct it, so a missing field fails loudly at the call site instead of surfacing as a `nil` three functions later. It is not a type system — fields aren't typed and nothing is checked at compile time; the check is a value-level guard at construction. `Record.new` is the recoverable twin (`{'ok', rec}` / `{'error', reason}`).

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

### One supervised process per connection

`supervise(...)` fixes its children at start. But an agent usually doesn't know its children up front: a voice agent gets one call at a time but many over its life; a server gets one request per socket. Don't re-`supervise()` per event (it rebuilds the whole tree) and don't `spawn()` bare (an unsupervised crash is silent). Start one **dynamic supervisor** at boot and add a child per event:

```sw
fun serve(sup) {
    socket = accept_next()                                   # blocks for the next call/request
    sup_start_child(sup, {nil, fun() { handle(socket) }, 'temporary'})
    serve(sup)
}

fun main() {
    sup = dyn_supervisor()
    serve(sup)
}
```

`dyn_supervisor()` starts empty and is always `one_for_one` (each child independent). `sup_start_child(sup, {name, fn, restart})` takes the same `{name, fn, restart}` tuple as `supervise` — `name` may be `nil` for anonymous workers — and returns the child's pid (or `nil`). `:temporary` is the right policy for per-connection workers (a dropped call shouldn't be retried); use `:transient` if a crash, not a clean hangup, should reconnect. If a child crash-loops past `max_restarts` within `max_seconds` (default 3 / 5s, tunable via `dyn_supervisor(max_restarts, max_seconds)`), the supervisor kills its children and shuts down — the same circuit breaker as `supervise`, so a runaway child can't restart forever.

Like all process primitives, these run only in compiled binaries (`swc build`). Under the interpreter (`swc run`, `swc test`, the REPL) they warn and return `nil`.

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
# Always bind values as parameters — never interpolate them into SQL.
# db_exec(h, sql, [args]) prepares + binds + steps, so user_input can
# never break out of the string and inject SQL.
db_exec(db, "INSERT INTO conv (role, turn) VALUES (?, ?)", ["user", user_input])
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
module Scheduler
import Cron

# Hand Cron a callback. An inline lambda `fn() { ... }` is a first-class value,
# so it works directly as the argument — or pass a named function by name when
# you want to reuse the work elsewhere. Both forms are shown below.
fun send_daily_report() { print("daily report") }

fun main() {
    Cron.every(30000, fn() { print("checking inbox") })  # every 30 sec, inline lambda
    Cron.at("09:00", send_daily_report)                  # daily at 09:00 local, named fn
    Cron.in_ms(5000, fn() { print("one-shot timer") })   # one-shot in 5 sec
}
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

## Seeing the swarm: `Swarm.top()` / `Swarm.tree()`

Telemetry is what your agent *emits*; `Swarm` is what it can *observe about itself* right now. When you've spawned a swarm of agents and tool workers, you usually want the equivalent of `top` and a supervision tree:

```sw
import Swarm

Swarm.top()     # list of %{pid, status, messages, name?, parent?} for every live process
Swarm.tree()    # the supervision forest: each root + its children, recursively

Swarm.print_top()    # prints a flat table, returns the count
Swarm.print_tree()   # prints an indented tree, returns the root count
```

`top()` is a flat snapshot; `tree()` reconstructs the hierarchy from each process's parent link (set by `spawn_link` / the supervisors), so a worker started with `sup_start_child` shows up nested under its supervisor. Use it to drive a LiveView dashboard, log a periodic census, or just answer "what's alive and who's supervising it?" while debugging.

It's a thin pure-sw library over the `process_list` / `process_info` builtins — and like all process primitives it's compiled-only (`swc build`); under the interpreter the underlying reads return `nil`. The reads are best-effort and lock-free (the same contract as the deadlock watchdog), so `messages` is the drained-mailbox depth, not every byte in flight — fine for observability, don't build control flow on exact counts.

---

## Self-defined tools: an agent that writes its own code

This is the one most agent runtimes can't do. A swarmrt binary ships the `sw` interpreter *inside it*, so an agent can write a brand-new tool **as `sw` source at runtime** and call it live — no recompile, no restart, no separate process:

```sw
# the model emitted this source (a string); register it as a named tool
src = "module T\nfun run(text) { ... summarize text ... }"
case tool_define("summarize", src) {
    'ok'            -> tool_call("summarize", article)
    {'error', why}  -> retry_with_feedback(why)   # bad source fails LOUDLY
}
```

The loop that makes this powerful: the model proposes a tool, you `tool_define` it, and if the source is malformed you get `{'error', reason}` back — feed that straight to the model as a compile error and let it fix its own tool. A tool that references something that doesn't exist, or omits `fun run(...)`, is rejected at define time, not discovered as a silent `nil` three turns later.

```sw
tool_define("scale", "module T\nfun run(x) { x * 3 }")
tool_call("scale", 7)              # 21
tool_define("scale", "module T\nfun run(x) { x * 10 }")   # hot-swap, live
tool_call("scale", 7)             # 70 — no restart
tool_rollback("scale")            # back to *3 if the new one misbehaves
tool_list()                       # [{scale, 3}, ...] — every tool + version
```

The design keeps it honest and safe-ish by construction: tools are **pure logic** (process primitives degrade to `nil` inside a tool, so a tool can't quietly spawn, send, or receive), they hold **no state of their own** (state lives in the caller, ETS, or SQLite, so a reload never corrupts memory), and every version is retained for `tool_rollback`. A tool's `run` can use the full language — helpers, recursion, `case`, `map`/`filter`, f-strings — and each `tool_call` runs in an isolated interpreter, so concurrent callers and a faulting call can't corrupt or poison each other. It's compiled-only — the interpreter that runs the tool is only live in a `swc build` binary.

**Safe self-extension.** Because the source is machine-generated and untrusted, `tool_define` runs an **admission lint** before registering: a call to a function that isn't a builtin or defined in the tool is rejected *loudly at define time* (a hallucinated/typo'd name is `{'error', "unknown function ..."}`, not a silent `nil` three calls later), and dangerous builtins are **default-deny** — `file_*`, `db_*`, and `shell`/`exec_argv` require an explicit grant:

```sw
tool_define("reader", "module T\nfun run(p) { db_query(p, \"select 1\") }", ['db'])   # granted
tool_define("sneaky", "module T\nfun run() { shell(\"rm -rf /\") }")                    # → {'error', needs 'shell'}
```

The grant is checked statically (including module-level `let` globals, so a tool can't smuggle `let x = shell(...)` past it), and `sys_exit` is forbidden outright (a tool can't kill the host). Pure logic — `map`/`filter`/recursion/lambdas/string+math/json — is always allowed, no caps needed.

Every version is kept as **replayable source** — `tool_history(name)` returns `[{version, src}, …]` (the last 16), so an agent's self-modification is fully auditable: nothing it wrote about itself is lost. (In-memory per session; persist it to SQLite yourself if you need it across restarts.)

Current limits (fast-follow): a tool can't call *another* tool (`tool_call` degrades to `nil` inside a tool — tools are leaf logic for now); each `tool_call` allocates its result without freeing intermediates (fine per turn; a tight 100k-call loop will grow memory); and compose tool source with `++`, not f-string `{{` braces.

The honest boundary: this is reload of an agent's *skills* (its tools), not its *running processes*. A tool you `tool_define` is callable on the next `tool_call`; a process already mid-call finishes on the version it started with. For updating the agent's *prompt/policy* as data, keep it in ETS/SQLite and re-read it each turn; for updating the *binary itself*, that's an operational restart (the runtime boots in <10ms).

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

→ **[`examples/http_echo.sw`](../examples/http_echo.sw)** (~30 lines)

### Request shapes — and reading request headers

`http_listen(port)` delivers every request to the calling process (or whichever
process `ws_set_handler` re-homed the connection to) as one of these messages:

```
{'http_request', conn, method, path, headers, body}   # plain HTTP
{'ws_connect',   conn, path}                           # a WS UPGRADE was accepted
{'ws_message',   conn, text}                           # a WS text frame
{'ws_binary',    conn, b64}                            # a WS binary frame (base64)
{'ws_close',     conn}                                 # the WS closed
```

`headers` is a **MAP with lowercased keys** — so a bearer-token check or a
webhook-signature read works in-process, regardless of on-the-wire casing:

```sw
{'http_request', conn, _method, _path, headers, _body} ->
    case map_get(headers, "authorization") {        # 'token' / "..." both match
        'nil'                  -> http_respond(conn, 401, [], "missing token\n")
        "Bearer s3cret"        -> http_respond(conn, 200, [], "ok\n")
        _                      -> http_respond(conn, 403, [], "bad token\n")
    }
```

Duplicate header names collapse to the last value. The map is bounded (≤128
headers, value ≤4 KB) — a hostile request can't blow it up.

**WebSockets carry their headers on the connection, not the message.** A WS
UPGRADE's `{'ws_connect', conn, path}` keeps its small 3-tuple shape (so existing
LiveView handlers are untouched); to read the headers the socket was opened with
— the `Origin`, an `Authorization`, the Telnyx `telnyx-signature-ed25519` /
`telnyx-timestamp` — query the connection:

```sw
{'ws_connect', conn, path} ->
    hdrs = ws_request_headers(conn)        # MAP, lowercased keys (always a map)
    origin = map_get(hdrs, "origin")       # CSRF / allow-list check before accept
    p      = ws_request_path(conn)         # same value as `path`, re-readable
    ...
```

`ws_request_headers/1` and `ws_request_path/1` stay valid for the life of the
socket, so a per-connection process that was handed the socket via
`ws_set_handler` can still pull them.

A real-world agent stack that uses all of this in anger:

→ **[swarm-code](https://github.com/skyblanket/swarm-code)** — a terminal coding agent (private repo, ask Sky for access)

---

## When the runtime feels wrong, it's usually because

- **You forgot a base case in your tail-recursive loop.** Stack doesn't grow, but the agent will spin if the recursion's terminating condition is never met. Always `case state { 'done' -> 'bye' ; _ -> agent(new_state) }` or similar.
- **You're holding state in a global instead of recursing.** sw has module-level `let` constants (read-only, declared at the top of a module), but no mutable globals. Use the recursion arg to carry mutable state, or ETS if it's actually shared across processes. The temptation to reach for a global usually means the design is muddled.
- **You're using `if/else` for what should be `case`.** If you find yourself with `else { if (x) { … } else { if (y) { … } } }`, that's a `case x { … }` waiting to be written.
- **Your agent's `receive` clauses don't have a catchall.** Unmatched messages stay queued forever — your agent will start growing memory invisibly. Either match exhaustively or end with `_other -> agent(state)`.
- **You're trying to make a synchronous HTTP call from inside a `receive` arm and it feels off.** It's fine — the process suspends on the syscall, the scheduler runs other processes meanwhile. That's what the runtime is for.

---

## See also

- [SW_LANGUAGE.md](SW_LANGUAGE.md) — full language reference
- [AGENT_SYSTEM.md](AGENT_SYSTEM.md) — cheatsheet for an LLM writing sw code (different audience: agents writing sw, not developers writing agents)
- [ARCHITECTURE.md](ARCHITECTURE.md) — runtime internals (schedulers, mailboxes, GC, distribution)
- [BENCHMARKS.md](BENCHMARKS.md) — process spawn, context switch, message send numbers
