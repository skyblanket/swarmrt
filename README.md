# SwarmRT

[![CI](https://github.com/skyblanket/swarmrt/actions/workflows/linux-quickstart.yml/badge.svg)](https://github.com/skyblanket/swarmrt/actions/workflows/linux-quickstart.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**A from-scratch BEAM-shaped runtime for the AI-agent era — written in C, compiled ahead of time, no VM, no GC pauses.**

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

It takes the parts of the BEAM (Erlang/Elixir's VM) that turned out to matter — lightweight processes, lock-free message passing, supervisors, hot reload, distribution — and reimplements them as a ~13K-line core C runtime + ~6K lines of studio builtins (HTTP, WebSocket, SQLite, JSON, files, etc.), plus a ~3K-line ahead-of-time compiler that emits native code. No interpreter. No bytecode. No VM warm-up. Each `.sw` file becomes a standalone executable that boots in <10ms and runs at native C speed.

(The full `src/` tree is ~42K lines; the rest is tests, benchmarks, three earlier prototype runtimes kept for reference, and tools like the search CLI and MCP server.)

It exists because the same workload BEAM was built for in 1986 — *thousands of long-lived, message-passing, partial-failure-tolerant processes* — is exactly what you need when you're running a swarm of AI agents. SwarmRT is the substrate behind [swarm-code](https://github.com/skyblanket/swarm-code) and a growing pile of agent-driven tools.

The language is called **`sw`** and is designed so an LLM can write it correctly on the first try. There's an [`eval/`](eval/) directory that measures this with real numbers: single-shot code-gen against 10 prompts × 3 models, no agent harness, no retries. Latest result: **Kimi K2.6 80%, Kimi K2.5 70%** ([results](eval/results/results.md)).

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
| **Building distributed systems** | Erlang-style multi-node TCP distribution. Supervisors with one-for-one / one-for-all / rest-for-one strategies. Hot code reload. Process linking and monitoring. |
| **Writing concurrent programs** | 100K+ lightweight processes per node. ~150ns context switches. Lock-free MPSC mailboxes. No `async`/`await` keyword salad — just `spawn` and `receive`. |
| **Avoiding language overhead** | One binary, no VM, no GC pauses (per-process generational GC means no global stop-the-world), <10ms startup. The binary statically links libswarmrt and dynamically links the four system libs above — no runtime install or VM image. |

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
| **[docs/BENCHMARKS.md](docs/BENCHMARKS.md)** | Spawn / send / context-switch numbers. |
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
- **The whole language fits in one document.** SW_LANGUAGE.md is ~600 lines including examples — small enough to paste into a system prompt.

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
| **`case`** | Tool-call dispatch: `case tool_name { "read" -> ... ; "bash" -> ... ; _ -> ... }`. |

> Hot reload is implemented in the runtime (`sw_module_register` / `sw_module_upgrade`) but is currently only reachable from **C embedders** — there is no `sw`-level builtin yet. See [docs/API_REFERENCE.md §12](docs/API_REFERENCE.md#12-hot-code-reload).

**Read more:** [docs/BUILDING_AGENTS.md](docs/BUILDING_AGENTS.md) — the developer-facing guide with the patterns.

**Working example:** [examples/llm_agent.sw](examples/llm_agent.sw) — a real LLM-driven agent in ~90 lines (prompt → http_post_stream → parse tool tags → `case` dispatch → loop). Set `API_KEY` and run; works against any OpenAI-compatible endpoint.

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
| **Process** | 2KB arena-allocated PCB + 64KB stack. Lock-free MPSC mailbox. Per-process generational GC. |
| **Behaviours** | GenServer, Supervisor, Task, GenStateMachine, ETS, Registry — all built on top of the bare `spawn`/`send`/`receive` primitives. |
| **IO** | kqueue-based async ports. TCP accept/read/write as port messages. HTTP / WebSocket / Chrome DevTools as builtins. |
| **Distribution** | Multi-node TCP message routing with automatic reconnection. Process linking across nodes. |
| **Hot reload** | Module versioning, swap running code without stopping processes. *(C API only — no `sw`-level builtin yet.)* |
| **Compiler (`swc`)** | `.sw` → AST → C → native binary. Tail-call optimisation, optional XOR-string obfuscation, optional symbol stripping. |

Numbers: process spawn ~100-500ns, context switch ~150ns, message send ~10ns (pointer sharing), 100K+ concurrent processes per node. Full breakdown in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

### Runtime env vars

Every `swc`-built binary picks these up at boot — no code changes
needed.

| Var | Default | Effect |
|---|---|---|
| `SW_SCHEDULERS` | CPU count | Number of scheduler threads. `1` for deterministic CLI tools. |
| `SW_MAX_PROCS` | `100000` | Arena ceiling. Drop to `1024`/`4096` for fast-start CLI binaries — saves ~20 ms boot. Floor of 16. |
| `SW_QUIET` | unset | Suppress the `[SwarmRT] Arena initialized…` banner on stderr. Set in scripts/CI. |

---

## Compiler CLI

```
swc build <file.sw> [-o <name>]   Compile to native binary
swc emit  <file.sw>               Print generated C to stdout
swc repl                          Interactive REPL (no file needed)
swc test [<file.sw>|<dir>]        Run test_* functions in .sw files
swc lsp                           Language Server (LSP 3.17 over stdio)

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

Variables persist across lines. Multi-line input continues until brackets balance. The REPL uses a tree-walking interpreter and supports the language core, the stdlib, and **most pure-functional builtins**: strings, JSON, maps, formatting, `case`, `try/catch`, files (`file_read/write/exists/list/mkdir`), SQLite (`db_open/exec/query`), one-shot shell (`shell`), `panic`, `expect`, `error`, `sleep`, `random_int`, `getenv`, `string_replace`/`sub`/`truncate`, `map_merge`/`remove`, `json_get`/`escape`, and the `Std`/`Mcp`/`Vec`/`Embed`/`Prompt` modules.

Process-scheduler primitives (`spawn`, `link`, `monitor`, `send`, `receive`, `trap_exit`, HTTP server, WebSocket, browser automation) still need the compiled path — the REPL doesn't simulate the full scheduler. Hit one of those names and the REPL prints a one-shot hint and returns `nil` instead of dropping through to "undefined function".

For everything else: write a `.sw` file and `swc build` it.

### Editor support

- **Tree-sitter grammar** at [`tree-sitter-sw/`](tree-sitter-sw/) covers the full language. Drop into Helix / Neovim / VS Code for syntax highlighting — see the directory's README for the wiring.
- **LSP** via `swc lsp`. Speaks LSP 3.17 over stdin/stdout, surfaces parse errors as diagnostics (red squigglies). Hook into your editor's LSP client.

### Standard library

The [`lib/`](lib/) directory ships modules that auto-resolve via `import` — no copy-paste, no manifest. Just write `import Std` and the compiler finds `<swarmrt>/lib/Std.sw`.

| Module | What it gives you |
|---|---|
| `Std` | List / map / string helpers (range, take, drop, nth/at, zip, partition, sort, unique, find, any, all, sum, product, group_by, chunk_every, intersperse, …) — see `lib/Std.sw` for the full list. |
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
| `mathlib.sw` + `math_test.sw` | Multi-module program with imports. |
| `supervisor.sw` | Restart strategies in action. |
| `mapreduce.sw` | Spawn a fan-out worker pool and collect results. |
| `distributed.sw` | Multi-node — start two `swarms` and pass messages over TCP. |
| `dispatcher.sw` | Studio-pattern actor: tagged messages routed via `case`, state via recursion arg. |
| `json_pipeline.sw` | JSON load → `case` classify → f-string render. The new ergonomics in 35 lines. |
| `http_echo.sw` | A working HTTP server in 25 lines — `case` on the path, f-strings for templating. |
| `llm_agent.sw` | A real LLM-driven agent in 90 lines. Prompt → http_post_stream → parse `<tool>` tags → `case` dispatch → loop. Works against any OpenAI-compatible endpoint. |

Compile and run any with `./bin/swc build examples/<name>.sw -o /tmp/x && /tmp/x`.

---

## Tests

```bash
make test-sw         # sw-language tests (covers builtins, processes, parser fixes)
make test-all        # backward-compat alias for test-core
make test-full       # the comprehensive gate: core + OTP + phases 2-10 + search + tools
```

`test-sw` runs the `tests/sw/test_*.sw` suite via `tests/sw/run_tests.sh`. It now covers two execution paths:

- **Compiled** — each `test_*.sw` is compiled with `swc build` and the resulting binary is run. ~110 assertions across 8 files.
- **Interpreter** — `tests/sw/repl/test_*.sw` files are run via `swc test` (tree-walking interpreter). Guards against the REPL/codegen builtin drift that the May 2026 marathon closed.

Add a `test_<topic>.sw` file in either directory and it'll be picked up automatically.

---

## LLM eval

The [`eval/`](eval/) directory is an empirical benchmark of how well LLMs write `sw` from the published docs. Pure code-gen, single-shot, no agent harness — measures the floor.

```bash
export MOONSHOT_KEY=...           # or whichever endpoint's key
cd eval && ./runner.sh            # 10 prompts × 3 models, ~20 min
```

Each prompt is a self-contained task with a deterministic stdout check. The runner extracts the LLM's `.sw` from a code fence, compiles it with `swc build`, runs it, and diffs stdout against the prompt's expected output. Per-run results land in `eval/results/<id>/summary.md`; the latest is mirrored to [`eval/results/results.md`](eval/results/results.md).

The point is to surface real gaps — the first baseline run flagged several quirks (nested case-as-RHS, f-string `f` prefix, pipe + module-prefix codegen) that are now tracked as fix-its.

---

## Build

Requires: a C compiler (cc/clang/gcc) and pthreads. Developed and tested on macOS Apple Silicon; Linux support is the intent (pthreads + posix-only APIs) but hasn't been continuously verified.

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
  swarmrt_gc.{c,h}         Per-process generational GC
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

Stable enough to be the substrate for [swarm-code](https://github.com/skyblanket/swarm-code). Daily-driven on macOS Apple Silicon; Linux x86_64 builds and runs in CI ([`.github/workflows/linux-quickstart.yml`](.github/workflows/linux-quickstart.yml)). Windows is best-effort.

**What CI gates on, every push:**
- README quickstart + a few example programs
- `make test-sw` — 9 files, **110 compiled + 16 interpreter assertions** (`.sw` language)
- `make test-phase{2..10}` — **9 C-side runtime test files**, all 100% green: GenServer/Supervisor (phase 2), ETS (phase 3, 15 tests), Agent/App/DynSup (phase 4, 14), StateMachine/ProcessGroup (phase 5, 12), TCP (phase 6, 6), hot reload (phase 7, 5), GC (phase 8, 5), distribution (phase 9, 4), language frontend (phase 10)
- `make test-full` — **comprehensive gate**: core + OTP + phases 2-10 + search + MCP + sws
- `make stress` — **100 runs total** (50 multi-scheduler + 50 single-scheduler), 80k spawns each, must clear 90% threshold

**Known issues:** one spawn-storm race in the message-send path on Linux x86_64 (single-scheduler reproduces; ~16% crash rate at 80k spawns under stress). Not the original ctx-tear race (closed deterministically in round 5 with a per-slot generation counter + ctx_lock); this one is a heap-corruption-then-strdup pattern in the mailbox/atom-allocator path. Tracked in [docs/notes/KNOWN_ISSUES.md](docs/notes/KNOWN_ISSUES.md).

**Reliability backstory:** the runtime has been through six rounds of external review (Claude web agent + Codex), each filing a markdown report against the latest commit. The full hardening narrative — what each round found, what was fixed, what's still open — is at [docs/notes/REVIEW_HARDENING.md](docs/notes/REVIEW_HARDENING.md).

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
