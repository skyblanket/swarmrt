<img src="docs/assets/banner.png" alt="swarmrt — erlang's soul, compiled. A BEAM-shaped runtime in C. Up to 100,000 processes. MIT." width="100%"/>

# SwarmRT

[![CI](https://github.com/skyblanket/swarmrt/actions/workflows/linux-quickstart.yml/badge.svg)](https://github.com/skyblanket/swarmrt/actions/workflows/linux-quickstart.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-C81E0F.svg)](LICENSE)

**A from-scratch BEAM-shaped runtime for the AI-agent era — written in C, compiled ahead of time, no VM, no GC pauses.**

https://github.com/skyblanket/swarmrt/raw/main/docs/assets/swarmrt-film.mp4

> **60 seconds, sound on** — the whole pitch: 99,990 processes alive on a laptop, crashed workers respawned by supervisors, sub-250 ns context switches. Every number measured, receipts in the repo.

In plain terms: Erlang's superpower — hundreds of thousands of cheap, crash-isolated processes passing messages, with supervisors that restart the ones that die — but compiled straight to a single native binary. No VM to install, no garbage-collector pauses, boots in milliseconds. The language on top (`sw`) is shaped so an LLM writes it correctly on the first try, because the point is running swarms of AI agents, each one its own process.

```sw
module Counter
export [main, counter]

fun counter(start) {
    receive {
        {'increment', by} -> counter(start + by)
        {'get', from}     -> send(from, {'count', start}) ; counter(start)
        'stop'            -> print("Counter stopped at " ++ start)
    }
}

fun main() {
    pid = spawn(counter(0))
    send(pid, {'increment', 5})
    send(pid, {'increment', 3})
    send(pid, {'get', self()})

    receive { {'count', n} -> print("Count: " ++ n) }
    send(pid, 'stop')
}
```

```bash
$ ./bin/swc build examples/counter.sw -o counter && ./counter
Count: 8
Counter stopped at 8
```

---

## What this is

SwarmRT is a runtime + language for writing concurrent programs that compile to a single native binary.

It takes the parts of the BEAM (Erlang/Elixir's VM) that turned out to matter — lightweight processes, lock-free message passing, supervisors, distribution — and reimplements them as a ~12K-line core C runtime + ~8.5K lines of studio builtins (HTTP, WebSocket, SQLite, JSON, files, etc.), plus a ~4K-line ahead-of-time compiler that emits native code. (A ~5.5K-line tree-walking interpreter powers the REPL, `swc run`, and the tests — but `swc build` compiles your `.sw` straight to native machine code: no bytecode, no VM warm-up.) Each `.sw` file becomes a standalone executable that boots in <10ms and runs at native C speed.

(The full `src/` tree is ~50K lines; the rest is C-side tests, benchmarks, three earlier prototype runtimes kept for reference, and tools like the search CLI and MCP server.)

It exists because the same workload BEAM was built for in 1986 — *thousands of long-lived, message-passing, partial-failure-tolerant processes* — is exactly what you need when you're running a swarm of AI agents. SwarmRT is the substrate behind [swarm-code](https://github.com/skyblanket/swarm-code) and a growing pile of agent-driven tools.

The language is called **`sw`** and is designed so an LLM can write it correctly on the first try. There's an [`eval/`](eval/) directory that measures this with real numbers — single-shot code-gen from the docs, no agent harness, no retries. Latest run (de-leaked prompt, 10 tasks, across vendors): **Claude Sonnet 4.5 100%, GPT-4.1 · Gemini 2.5 Flash · Kimi K2.6 all 90%, DeepSeek/Qwen 70%**, against a non-reasoning baseline at 30% ([full per-task table + per-attempt receipts](eval/results/results.md)). It's a small suite (n=10, single-shot, wide error bars), so read it as a floor, not a benchmark — but frontier models that never saw `sw` in training write it correctly first-try from the docs alone, which is the whole point.

---

## Quickstart (60 seconds)

```bash
# Install the C system libraries SwarmRT links against:
#   Ubuntu / Debian:
sudo apt-get install -y build-essential libsqlite3-dev libssl-dev zlib1g-dev
#   macOS (Homebrew):
brew install sqlite openssl@3 zlib

# Then:
git clone https://github.com/skyblanket/swarmrt && cd swarmrt
make swc libswarmrt          # builds the compiler + runtime library
./bin/swc build examples/counter.sw -o counter
./counter
```

That's it. No package manager for the language, no language server install, no VM image. The compiler is one binary and the runtime is one static library.

**Dependencies (small list, all in every major distro):** `cc` (clang or gcc) + pthreads (libc), plus four system libraries — `-lsqlite3`, `-lssl -lcrypto`, `-lz`, `-lm`. `sqlite` powers `db_*` builtins, openssl powers the WebSocket handshake, zlib is for PDF decompression, libm is for codegen-emitted math. If you want a truly minimal build later, those modules can be feature-flagged off.

---

## Why you might care

| If you're… | What SwarmRT gives you |
|---|---|
| **Running AI agents** | First-class actor model so each agent is a process. Selective receive for tool replies. ETS for shared state. HTTP / WebSocket / Chrome DevTools builtins so an agent can call APIs and drive a browser without spawning a Node sidecar. |
| **Building distributed systems** | Erlang-style multi-node TCP distribution. Supervisors with one-for-one / one-for-all / rest-for-one strategies. Process linking and monitoring. Versioned module registry with rollback for C embedders (no `sw`-level hot reload yet — see below). |
| **Writing concurrent programs** | 100K+ lightweight processes per node. ~150ns context switches. Lock-free MPSC mailboxes. No `async`/`await` keyword salad — just `spawn` and `receive`. |
| **Avoiding language overhead** | One binary, no VM, no GC pauses, <10ms startup. (Memory model — **Ownership v2**: each sw process has a **value arena** freed when the process exits. Escaped values get a lifecycle owner: a **sent message** is deep-copied into a region the receiver *adopts* on match (and reclaims at exit / next turn); **spawn arguments** are adopted into the child's arena; and a long-lived tail-recursive loop is bounded by a **scoped turn-checkpoint** that reclaims each turn's garbage above the function's entry floor. So memory stays bounded under fixed concurrency: a fixed-width spawn workload with 32 KB args, a fixed-depth large-message stream, and a 100k-turn loop all hold **flat** peak RSS as the job count scales 10× (`make gc-slope`). Cross-process messages are deep-copied (BEAM no-shared-heap). **Remaining caveat:** ETS entries, supervisor child closures, and timer/cron closures are still global-heap (reclaimed at OS exit / table destroy); the interpreter has no arena (short-lived). The binary statically links libswarmrt and dynamically links the four system libs above — no runtime install or VM image. |

---

## The language in one screenful

```sw
module Demo
export [main]

# Spawn a worker, send it work, get a reply.
fun worker() {
    receive {
        {'square', n, from} ->
            send(from, {'result', n * n})
            worker()
        'stop' -> 'done'
    }
}

fun main() {
    pid = spawn(worker())

    # Pipeline: build a list, map send over it, collect replies.
    [1, 2, 3, 4, 5] |> each(fun(n) { send(pid, {'square', n, self()}) })
    results = collect(5, [])
    send(pid, 'stop')

    print(f"squares: {results}")
}

fun collect(n, acc) {
    if (n == 0) { acc }
    else {
        receive { {'result', r} -> collect(n - 1, list_append(acc, r)) }
    }
}

fun each(lst, f) {
    if (length(lst) == 0) { 'ok' }
    else { f(hd(lst)) ; each(tl(lst), f) }
}
```

That's most of the syntax surface: `module`, `fun`, `spawn`, `send`, `receive`, `case`, `if/else`, `try/catch`, atoms (`'ok'`), tuples (`{...}`), lists (`[...]`), maps (`%{key: val}`), the pipe (`|>`), pattern matching in `receive` and `case` arms, and f-strings (`f"hi {name}"`).

Full reference: **[docs/SW_LANGUAGE.md](docs/SW_LANGUAGE.md)**.

---

## Documentation

| Doc | What it covers |
|---|---|
| **[docs/SW_LANGUAGE.md](docs/SW_LANGUAGE.md)** | The `.sw` language — syntax, types, processes, builtins, gotchas. Start here if you're writing sw code. |
| **[docs/BUILDING_AGENTS.md](docs/BUILDING_AGENTS.md)** | Using sw to build AI agents — process-as-agent, tool dispatch, streaming LLM, studio pattern, supervisors. Start here if that's what you're shipping. |
| **[docs/AGENT_SYSTEM.md](docs/AGENT_SYSTEM.md)** | Cheatsheet for an LLM that's *writing* sw on demand — load this into a system prompt, not into a human's head. |
| **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** | Internals — schedulers, mailboxes, GC, distribution. |
| **[docs/API_REFERENCE.md](docs/API_REFERENCE.md)** | The C runtime API — for embedding swarmrt or writing new builtins. |
| **[docs/DEPLOYMENT.md](docs/DEPLOYMENT.md)** | Running a compiled binary in production — platforms, the full env-var config reference, graceful shutdown, restart/recovery, health endpoints, backup/DR. |
| **[docs/BENCHMARKS.md](docs/BENCHMARKS.md)** | Spawn / send / context-switch numbers. |
| **[docs/RELEASING.md](docs/RELEASING.md)** | Versioning (SemVer) + how a release is cut. |
| **[docs/CHANGELOG.md](docs/CHANGELOG.md)** | What changed and when, with motivation. |

---

## What makes `sw` agent-friendly

Most languages were designed for humans first; LLM ergonomics are a happy accident. `sw` is the other way around — every syntax decision was made to maximise the chance that a model writes correct code on the first attempt:

- **No indentation sensitivity.** Brace-delimited blocks. The model can't get the columns wrong because columns don't matter.
- **Keywords over symbols.** `spawn`, `receive`, `send`, `case` instead of `!`, magic operators. Easier to recall, easier to grep.
- **Statement separator is either newline or `;`** — both work in any block (function body, if/else, receive arm, case arm). Models trained on C-family code don't get tripped up.
- **C reserved words are legal identifiers.** Use `inline`, `static`, `register` as variable names; the codegen mangles them silently.
- **One way to do most things.** No three-flavours-of-async. `spawn` + `receive` covers it.
- **`case` for top-level pattern matching.** No nested-if-else ladders. Same arm shape as `receive`, supports guards and falls through when a guard rejects.
- **f-strings + `format()`.** `f"req={req_id} ms={elapsed}"` instead of `"req=" ++ to_string(req_id) ++ " ms=" ++ to_string(elapsed)`. Composite values (tuples, lists, maps) render correctly.
- **Compile errors point at the exact failing line** via per-statement `#line` directives. The C compiler's message tells you `src/Module.sw:42`, not `/tmp/swc_xxx.c:16000`.
- **Compile-time "did you mean?"** for unknown function names. `unknown function 'strng_length' — did you mean 'string_length'?` Levenshtein over builtins + module funcs.
- **Loud runtime failures with full stack traces.** `hd([])`, `elem(t, 99)`, `n / 0` panic with `at src/X.sw:N` and the full call chain (`outer → middle → deep`). `expect(value, msg)` is the idiomatic unwrap. `try/catch` for recoverable cases.
- **A real stdlib in sw, auto-imported.** `import Std` (list / map / string helpers) just works from any project — swc falls back to `<swarmrt>/lib/`. Same goes for `Mcp`, `Embed`, `Vec`, `Prompt`, `Cron`, `Telemetry`.
- **Module-level `let` globals.** Top-level constant bindings at module scope — `let base_url = "https://api.example.com"` — shared by all functions in the module without threading them as arguments. *(Interpreter / REPL path only; codegen support is not yet wired — compiled binaries resolve the name as an atom. Use a top-of-function `let` binding or a module function returning the constant in compiled code.)*
- **`exec_argv(cmd, args)` builtin.** Fork+exec with no shell — safe for user-supplied arguments, no injection risk. Works in both the interpreter and `swc build` compiled code. (Use `shell_sandboxed` when you need a sandboxed shell for agent tool dispatch.)
- **`assert_raises(fn, msg)` builtin.** In `swc test` files, assert that a zero-arg lambda panics (or errors) with a message containing `msg`. The test runner intercepts the panic so the suite continues.
- **The whole language fits in one document.** SW_LANGUAGE.md is ~950 lines including examples — small enough to paste into a system prompt.

If you've watched an LLM struggle with Erlang's `case ... of -> ;`, with Rust's lifetimes, or with Python's import-vs-from-import-vs-as ceremony, `sw` is the reaction.

---

## Building AI agents

The reason swarmrt exists. If you've ever built an agent in Python with threading + asyncio + a tool-call parser + a retry layer + a mailbox abstraction stitched on top of `queue.Queue`, you've reinvented bad versions of what sw gives you in one binary.

| Primitive | Why an agent author cares |
|---|---|
| **process = agent** | `spawn()` one per agent. Mailbox is its inbox. Recursion is its state. No threads, no async/await. |
| **selective receive** | Agent A asks B a question, A blocks specifically on `{'reply', my_id, _}` — other messages stay queued. Zero callback hell. |
| **`http_post_stream`** | Streams LLM tokens with spinner + ESC-interrupt + reasoning-channel rendering for thinking-mode models. |
| **Subagent-mode streaming** | `http_post_stream(url, hdrs, body, parent_pid, name)` routes chunks as `{'stream_chunk', name, text}` to a parent — so `parallel([a, b, c])` doesn't interleave on the TTY. |
| **`Mcp` module** | MCP client + server (JSON-RPC over stdio) so sw agents can both consume any MCP tool AND expose their own tools as MCP. |
| **`wsc_*` WebSocket client** | For streaming APIs, WS-based LLM servers, custom RPC. |
| **`chrome_launch` + CDP** | Drive a real browser without Playwright/Node sidecar. |
| **`Vec` + `Embed`** | Vector memory: cosine-similarity store backed by ETS, embeddings via any OpenAI-compatible endpoint. |
| **`db_*` (SQLite)** | Embedded structured store — conversation history, todos, telemetry rollups. |
| **`Cron` module** | `Cron.every(ms, fn)` / `Cron.at("HH:MM", fn)` for autonomy loops + scheduled work. |
| **`Telemetry`** | One emit point, pluggable sinks (stdout / JSONL / your own). Real observability. |
| **`Prompt` templates** | `Prompt.render("Hello {{name}}", %{name: "Alice"})` — no more giant inline strings. |
| **ETS** | Agent registry, perms cache, conversation memory, todo state. |
| **`supervise` + `link` + `monitor` + `trap_exit`** | Full OTP fault tolerance from userland. Crash → restart → DOWN messages → `{'EXIT', from, reason}` for trappers. |
| **Sandboxed shell** | `shell_sandboxed(cmd, opts)` — sandbox-exec on macOS, firejail on Linux. Network blocked by default. |
| **`exec_argv(cmd, args)`** | Fork+exec without a shell — safe for user-supplied args (no injection risk). Returns `{exit_code, output}`. Works in the interpreter and compiled code. |
| **`case`** | Tool-call dispatch: `case tool_name { "read" -> ... ; "bash" -> ... ; _ -> ... }`. |

> "Hot reload" here is a C-embedder-only module registry (`sw_module_register` / `sw_module_upgrade`): it re-points a named slot to **another C function already compiled into the binary** — versioned, with rollback — and notifies tracked processes. It does **not** load new code, and there is no `sw`-level builtin: because every `.sw` function AOT-compiles to a fixed C symbol with no `dlopen`/bytecode loader, a compiled sw agent's own code cannot be swapped at runtime. See [docs/API_REFERENCE.md §12](docs/API_REFERENCE.md#12-hot-code-reload).

**Read more:** [docs/BUILDING_AGENTS.md](docs/BUILDING_AGENTS.md) — the developer-facing guide with the patterns.

**Working example:** [examples/llm_agent.sw](examples/llm_agent.sw) — a real LLM-driven agent in ~170 lines (prompt → http_post_stream → parse tool tags → `case` dispatch → loop). Set `API_KEY` and run; works against any OpenAI-compatible endpoint.

**Real-world stack:** [swarm-code](https://github.com/skyblanket/swarm-code) — a terminal coding agent that uses all of the above in anger.

---

## The runtime, briefly

```
┌─────────────────────────────────────────────────┐
│                   sw_swarm_t                     │
│  ┌───────────┬───────────┬───────────┐          │
│  │ Scheduler │ Scheduler │ Scheduler │  ...     │
│  │  thread 0 │  thread 1 │  thread N │          │
│  └─────┬─────┴─────┬─────┴─────┬─────┘          │
│        │           │           │                 │
│   ┌────▼────┐ ┌────▼────┐ ┌────▼────┐           │
│   │ Run Q   │ │ Run Q   │ │ Run Q   │           │
│   │ [P P P] │ │ [P P P] │ │ [P P P] │           │
│   └─────────┘ └─────────┘ └─────────┘           │
│                                                  │
│  Arena: single mmap, partitioned per-scheduler   │
│  Registry: lock-free named process lookup        │
│  Timers: sorted list with millisecond resolution │
└──────────────────────────────────────────────────┘
```

| Subsystem | What it does |
|---|---|
| **Scheduler** | One OS thread per core. Per-scheduler run queue with 4 priority levels. Reduction-counted preemption. Work stealing between cores. |
| **Process** | 2KB arena-allocated PCB + 128KB stack. Lock-free MPSC mailbox. Per-process **value arena** freed at process exit. **Ownership v2:** sent messages + spawn args are deep-copied into regions the receiver/child *adopts* and reclaims; long-lived tail loops are bounded by a scoped turn-checkpoint. Cross-process messages deep-copied (no shared heap). (ETS values still global-heap until table destroy.) |
| **Behaviours** | GenServer, Supervisor, Task, GenStateMachine, ETS, Registry — all built on top of the bare `spawn`/`send`/`receive` primitives. |
| **IO** | kqueue-based async ports. TCP accept/read/write as port messages. HTTP / WebSocket / Chrome DevTools as builtins. |
| **Distribution** | Multi-node TCP message routing with automatic reconnection. Process linking across nodes. |
| **Hot reload** | Versioned module registry with rollback: re-points a named slot to another C function already compiled into the binary and notifies tracked processes — does not load new code. *(C API only — no `sw`-level builtin; compiled `.sw` code can't be swapped at runtime.)* |
| **Compiler (`swc`)** | `.sw` → AST → C → native binary. Tail-call optimisation, optional XOR-string obfuscation, optional symbol stripping. |

Numbers: process spawn ~100-500ns, context switch ~150ns, message send ~10ns to enqueue plus a deep-copy of the payload into the receiver (BEAM-style no-shared-heap — O(message size); small messages stay cheap), 100K+ concurrent processes per node. Full breakdown in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

### Runtime env vars

Every `swc`-built binary picks these up at boot — no code changes
needed.

| Var | Default | Effect |
|---|---|---|
| `SW_SCHEDULERS` | CPU count | Number of scheduler threads. `1` for deterministic CLI tools. |
| `SW_MAX_PROCS` | `100000` | Arena ceiling. Drop to `1024`/`4096` for fast-start CLI binaries — measured ~6 ms total wall (vs ~40 ms at the default ceiling) on Linux x86_64. Floor of 16. |
| `SW_MAILBOX_MAX` | `1000000` | Mailbox depth cap (pending messages per process). User sends over the cap are dropped — loudly: a global counter (`sw_mailbox_dropped()`) plus a rate-limited stderr warning. EXIT/DOWN signals and timer fires are exempt, so supervision and `receive … after` keep working on a flooded process. `0` disables. |
| `SW_HTTP_MAX_REQUEST` | `33554432` (32MB) | Max bytes buffered for one HTTP request (headers + body) and max accepted `Content-Length`. Oversized declared bodies get a `413`; connections that outgrow the buffer cap are closed. Min `4096`. |
| `SW_HTTP_IDLE_TIMEOUT_MS` | `30000` | Close an HTTP connection idle (no inbound bytes) this long and free its slot (slow-loris defense). `0` disables; established WebSockets are exempt unless `SW_HTTP_WS_IDLE_TIMEOUT_MS` is set. |
| `SW_PROC_MEM_MAX` | `0` (off) | Per-process memory cap (bytes). A process over the cap dies loudly; the node + siblings survive. |
| `SW_MSG_MAX_BYTES` | `0` (off) | Max size of a single local message (bytes). Over-cap sends are dropped loudly, leak-free. |
| `SW_SHUTDOWN_GRACE_MS` | `5000` | Graceful-shutdown drain deadline. On SIGTERM/SIGINT the node stops accepting work, drains, cancels timers, then tears down within this budget. |
| `SW_LOG_JSON` | unset | `1` → one JSON `proc_crash` record per abnormal exit on stderr (for log shippers). Human-readable trace stays the default. |
| `SW_QUIET` | unset | Suppress the `[SwarmRT] Arena initialized…` banner on stderr. Set in scripts/CI. |

The full operational config reference (every var, defaults, meanings) and the
graceful-shutdown / recovery model live in **[docs/DEPLOYMENT.md](docs/DEPLOYMENT.md)**.

---

## Compiler CLI

```
swc build <file.sw> [-o <name>]   Compile to native binary
swc emit  <file.sw>               Print generated C to stdout
swc repl                          Interactive REPL (no file needed)
swc test [<file.sw>|<dir>]        Run test_* functions in .sw files
swc lsp                           Language Server (LSP 3.17 over stdio)
swc version                       Print the version (also --version, -v)

Options for build/emit
  -o <name>          Output binary name
  -O                 Optimise (-O2)
  --obfusc           XOR-encode string literals + mangle symbols
  --strip            Strip the symbol table
  --emit-c           Save the .gen.c next to the binary (useful for debugging codegen)
  --target=<triple>  Cross-compile (e.g. x86_64-linux-gnu, aarch64-apple-darwin).
                     Cross-macOS-arch works out of the box; non-darwin targets
                     need `zig` or a matching cross-gcc in PATH.
```

Imports are auto-resolved from `src/` next to the file you're compiling — no manifest, no lockfile.

### REPL

```
$ ./bin/swc repl
SwarmRT REPL v0.1 — type :help for commands, :quit to exit
sw> 2 + 3
5
sw> xs = [1, 2, 3, 4]
[1, 2, 3, 4]
sw> length(xs)
4
sw> case 7 { 0 -> "zero" ; n when n > 0 -> "positive" ; _ -> "neg" }
"positive"
sw> format("hi {} you are {}", "world", 30)
"hi world you are 30"
```

Variables persist across lines. Multi-line input continues until brackets balance. The REPL uses a tree-walking interpreter and supports the language core, the stdlib, and **most pure-functional builtins**: strings, JSON, maps, formatting, `case`, `try/catch`, files (`file_read/write/exists/list/mkdir`), SQLite (`db_open/exec/query`), one-shot shell (`shell`), `exec_argv`, `panic`, `expect`, `error`, `assert_raises`, `sleep`, `random_int`, `getenv`, `string_replace`/`sub`/`truncate`, `map_merge`/`remove`, `json_get`/`escape`, and the `Std`/`Mcp`/`Vec`/`Embed`/`Prompt` modules.

Process-scheduler primitives (`spawn`, `link`, `monitor`, `send`, `receive`, `trap_exit`, HTTP server, WebSocket, browser automation) still need the compiled path — the REPL doesn't simulate the full scheduler. Hit one of those names and the REPL prints a one-shot hint and returns `nil` instead of dropping through to "undefined function".

For everything else: write a `.sw` file and `swc build` it.

### Editor support

- **Tree-sitter grammar** at [`tree-sitter-sw/`](tree-sitter-sw/) covers the full language. Drop into Helix / Neovim / VS Code for syntax highlighting — see the directory's README for the wiring.
- **LSP** via `swc lsp`. Speaks LSP 3.17 over stdin/stdout, surfaces parse errors as diagnostics (red squigglies). Hook into your editor's LSP client.

### Standard library

The [`lib/`](lib/) directory ships modules that auto-resolve via `import` — no copy-paste, no manifest. Just write `import Std` and the compiler finds `<swarmrt>/lib/Std.sw`.

| Module | What it gives you |
|---|---|
| `Std` | List / map / string helpers (map, filter, join, range, take, drop, nth/at, zip, partition, sort, unique, find, any, all, sum, product, group_by, chunk_every, intersperse, string_join, …) — see `lib/Std.sw` for the full list. `map`/`filter`/`reduce` also exist as global builtins; `Std.map`/`Std.filter`/`Std.join` work too. |
| `Mcp` | Model Context Protocol client + server (JSON-RPC over stdio) |
| `Embed` | Embeddings client for any OpenAI-compatible `/v1/embeddings` endpoint |
| `Vec` | ETS-backed cosine-similarity vector store (`Vec.new / add / search / size`) |
| `Prompt` | `{{var}}` template engine — render from a string or a file |
| `Cron` | Wake scheduler — `Cron.every(ms, fn)` / `Cron.at("14:00", fn)` |
| `Telemetry` | Event hub with stdout / file / JSONL sinks |

---

## Examples

The [`examples/`](examples/) directory has small focused programs. Each shows one core feature:

| File | Shows |
|---|---|
| `hello.sw` | Minimal program — `print` and exit. |
| `counter.sw` | Process spawning, send, receive, pattern matching on tuples. |
| `pingpong.sw` | Bidirectional message passing between two processes. |
| `lambda.sw` | Anonymous functions and closures. |
| `multi_main.sw` | Multi-module program — imports `MathLib` (from `mathlib.sw`), calls `factorial` / `sum_list`. |
| `supervisor.sw` | Restart strategies in action. |
| `mapreduce.sw` | Spawn a fan-out worker pool and collect results. |
| `distributed.sw` | Multi-node — start two `swarms` and pass messages over TCP. |
| `dispatcher.sw` | Studio-pattern actor: tagged messages routed via `case`, state via recursion arg. |
| `json_pipeline.sw` | JSON load → `case` classify → f-string render. The new ergonomics in ~40 lines. |
| `http_echo.sw` | A working HTTP server in ~35 lines — `case` on the path, f-strings for templating. |
| `llm_agent.sw` | A real LLM-driven agent in ~170 lines. Prompt → http_post_stream → parse `<tool>` tags → `case` dispatch → loop. Works against any OpenAI-compatible endpoint. |

Compile and run any with `./bin/swc build examples/<name>.sw -o /tmp/x && /tmp/x`. (`mathlib.sw` is a library — it has no `main`, so build the importer `multi_main.sw` instead. `math_test.sw` is a test file: run it with `./bin/swc test examples/math_test.sw`.)

---

## Tests

```bash
make test-sw         # sw-language tests (covers builtins, processes, parser fixes)
make test-all        # backward-compat alias for test-core
make test-full       # the comprehensive gate: core + OTP + phases 2-10 + search + tools
```

`test-sw` runs the `tests/sw/test_*.sw` suite via `tests/sw/run_tests.sh`. It now covers two execution paths:

- **Compiled** — each `test_*.sw` is compiled with `swc build` and the resulting binary is run.
- **Interpreter** — `tests/sw/repl/test_*.sw` files are run via `swc test` (tree-walking interpreter). Guards against the REPL/codegen builtin drift that the May 2026 marathon closed.

Together the suite reports `all sw tests passed — 56 files, 493 assertions`, and `make test-sw` then runs the **dual-path conformance gate**: every program in `tests/sw/conform/` executes under BOTH `swc run` (interpreter) and `swc build` (compiled) and must produce byte-identical stdout and exit codes — the structural guard against the two paths drifting apart.

Add a `test_<topic>.sw` file in either directory and it'll be picked up automatically.

---

## LLM eval

The [`eval/`](eval/) directory is an empirical benchmark of how well LLMs write `sw` from the published docs. Pure code-gen, single-shot, no agent harness — measures the floor.

```bash
# Keys match models.json: Kimi uses MOONSHOT_KEY; GPT-4.1 / Gemini / DeepSeek /
# Qwen route through OpenRouter. Set either or both — an unset key SKIPs that model.
export OPENROUTER_API_KEY=...     # GPT-4.1, Gemini 2.5 Flash, DeepSeek, Qwen
export MOONSHOT_KEY=...           # Kimi K2.6 / K2.5 / moonshot-v1-32k
cd eval && ./runner.sh            # 10 prompts × the models in models.json, ~20 min
```

Each prompt is a self-contained task with a deterministic stdout check. The runner extracts the LLM's `.sw` from a code fence, compiles it with `swc build`, runs it, and diffs stdout against the prompt's expected output. Per-run results land in `eval/results/<id>/summary.md`; the latest is mirrored to [`eval/results/results.md`](eval/results/results.md).

The point is to surface real gaps — the first baseline run flagged several quirks (nested case-as-RHS, f-string `f` prefix, pipe + module-prefix codegen) that are now tracked as fix-its.

---

## Build

Requires: a C compiler (cc/clang/gcc) and pthreads. Developed and daily-driven on macOS Apple Silicon; Linux x86_64 is CI-gated (the README quickstart, examples, sw + phase tests, and stress run on every push — see [`.github/workflows/linux-quickstart.yml`](.github/workflows/linux-quickstart.yml)). Windows is best-effort.

```bash
make swc libswarmrt              # compiler + runtime library only (you usually want this)
make all                         # everything: compiler, library, examples, demos
make test-sw                     # sw-language tests
make test-all                    # backward-compat alias for test-core
make test-full                   # comprehensive gate (core + OTP + phases + search + tools)
make clean                       # nuke build artefacts
```

The compiler finds `libswarmrt.a` and headers relative to its own location, so `bin/swc` is portable — copy it anywhere.

---

## Project layout

```
src/
  swarmrt_native.{c,h}     Core runtime: scheduler, spawn, send/receive, arena
  swarmrt_asm.S            ARM64 context switching (register save/restore)
  swarmrt_otp.{c,h}        GenServer, Supervisor
  swarmrt_task.{c,h}       Task (async/await)
  swarmrt_ets.{c,h}        ETS tables
  swarmrt_phase4.{c,h}     Agent, Application, DynamicSupervisor
  swarmrt_phase5.{c,h}     GenStateMachine, Process Groups
  swarmrt_io.{c,h}         kqueue async I/O, TCP ports
  swarmrt_hotload.{c,h}    Hot code reload with versioning
  swarmrt_varena.{c,h}     Per-process value arena (GC v1 — freed on process exit)
  swarmrt_gc.{c,h}         Legacy GC scaffolding (unused; superseded by varena)
  swarmrt_node.{c,h}       Multi-node distribution
  swarmrt_lang.{c,h}       Lexer, parser, tree-walking interpreter
  swarmrt_codegen.{c,h}    AST → C code generation
  swarmrt_builtins_studio.h Builtins: HTTP, JSON, ETS, files, WS, Chrome, base64
  swarmrt_obfusc.c         String XOR encoding + symbol mangling
  swc.c                    Compiler CLI driver

examples/                  Small standalone .sw programs, one feature each
tests/sw/                  sw-language test files + run_tests.sh
docs/                      Long-form documentation
```

---

## Status

Stable enough to be the substrate for [swarm-code](https://github.com/skyblanket/swarm-code). Daily-driven on macOS Apple Silicon; both macOS ARM64 and Linux x86_64 build, test, and run the gate suite in CI ([`.github/workflows/linux-quickstart.yml`](.github/workflows/linux-quickstart.yml)); Linux ARM64 is built and released via the release workflow. Windows is best-effort.

**What CI gates on, every push:**
- README quickstart (`counter.sw`) + a few more example programs (`hello.sw`, `lambda.sw`)
- `bash scripts/check_sw_docs.sh` — **doc-compile tripwire**: every complete ```sw block in the docs and every runnable `examples/*.sw` must still compile with this `swc`
- `make test-sw` — **59 files, 514 assertions** (`.sw` language: compiled + interpreter + `swc run` paths) **plus the dual-path conformance gate** (`tests/sw/conform/` — interpreter and compiled output must be byte-identical per program)
- `make test-phase$p` for `p` in **2 through 10** — C-side runtime tests: GenServer/Supervisor (phase 2), ETS (phase 3), Agent/App/DynSup (phase 4), StateMachine/ProcessGroup (phase 5), TCP (phase 6), hot reload (phase 7), GC scaffolding (phase 8), distribution (phase 9), language frontend (phase 10); the **deadlock watchdog** runs automatically in every test (active by default in the runtime)
- `make stress` — high-process-count race guard (multi-scheduler + single-scheduler spawn storm); every run must complete
- `make gc-stress` — GC v1 copy-on-escape correctness: the value-arena stress harness compiled with ASAN + `-DSW_ARENA_POISON`; a missed deep-copy on any send/spawn/ETS boundary surfaces as a use-after-free or a `0xDE`-garbage content assert

- `make gc-slope` — **Ownership v2 memory-slope gate (CI, default + `SW_SCHEDULERS=1`)**: peak-RSS growth must stay under budget across a 10× scale-up of fixed-concurrency spawns (32 KB args), fixed-depth large messages, a 100k-turn tail loop, and pmap. Each probe only counts if it exits 0 and prints `PROBE_OK` (a crashing/`sys_exit(1)` probe fails the gate).
- `make fuzz` — untrusted-input boundaries under ASAN/UBSAN (parser, JSON, distribution unmarshal, HTTP headers, WebSocket frames, the SQLite arg path)
- **Operational-limit + isolation gates**, each **bidirectional** (proven to fail without its fix): `quota-gate` (per-process memory), `msgsize-gate` (message size), `slowloris-gate` (HTTP idle timeout), `isolation-gate` (a crashing process can't take down siblings/node), `crashlog-gate` (`SW_LOG_JSON`), `health-gate` (`/healthz` + `/readyz`), `shutdown-gate` (graceful drain, deadline-bounded even for a hung workload)

**Known limitations** (honest list — see [docs/notes/KNOWN_ISSUES.md](docs/notes/KNOWN_ISSUES.md) for repros):
- **Compiled `receive` has no default timeout.** A bare `receive` (no `after`) blocks forever in a compiled binary, while the interpreter defaults to a 5s timeout — so use an explicit `after MS` in compiled `receive`s that might not match, to avoid a silent divergence.
- **No static type or shape checking** — `sw` is dynamically typed by design. Typos in variable names compile to atoms rather than erroring (e.g. `undefined_var` becomes `:undefined_var`); there is no compile-time catch.
- **Memory is bounded under fixed concurrency (Ownership v2), with a few owners still on the global heap.** A process's own working set frees on exit; sent messages, spawn args, and pmap captures/results are adopted into receiver/child ownership and reclaimed (incl. pre-start-killed children); long-lived tail loops are bounded by a scoped turn-checkpoint. `make gc-slope` proves spawn / message / 100k-turn / pmap slopes stay flat. **Still global-heap (reclaimed only at OS exit / table destroy — a high-churn loop grows until then):** (1) **ETS** entries; (2) **supervisor child closures** and **timer/cron closures**; (3) the **interpreter** has no arena (fine for short-lived `swc run`/REPL). Values nested deeper than 256 levels are truncated on cross-process copy.

The previous Linux x86_64 spawn-storm race is closed as of the May 29 sushi re-test: 50/50 multi-scheduler and 50/50 single-scheduler runs completed with zero crashes. Any future miss in `make stress` should be treated as a regression.

**Reliability backstory:** the runtime has been through six rounds of external review (Claude web agent + Codex), each filing a markdown report against the latest commit. The full hardening narrative — what each round found and what was fixed — is at [docs/notes/REVIEW_HARDENING.md](docs/notes/REVIEW_HARDENING.md).

New runtime features land regularly — see [CHANGELOG](docs/CHANGELOG.md). Breaking changes are called out in the changelog and the language reference is the source of truth.

---

## Contributing

Issues and pull requests are welcome. [CONTRIBUTING.md](CONTRIBUTING.md)
covers the build, the test suite, and what CI gates on — run `make
test-sw` and the phase tests before opening a PR. By participating you
agree to the [Code of Conduct](CODE_OF_CONDUCT.md). Security reports go
through a private channel — see [SECURITY.md](SECURITY.md).

---

## License

MIT — built by [Otonomy](https://otonomy.ai).
