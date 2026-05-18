# SwarmRT

**A from-scratch BEAM-shaped runtime for the AI-agent era — written in C, compiled ahead of time, no VM, no GC pauses.**

```sw
module Counter
export [main, counter]

fun counter(n) {
    receive {
        {'inc', by} -> counter(n + by)
        {'get', from} -> send(from, {'count', n}) ; counter(n)
        'stop' -> n
    }
}

fun main() {
    pid = spawn(counter(0))
    send(pid, {'inc', 5})
    send(pid, {'inc', 3})
    send(pid, {'get', self()})

    receive { {'count', n} -> print("count: " ++ to_string(n)) }
    send(pid, 'stop')
}
```

```bash
$ ./bin/swc build counter.sw -o counter && ./counter
count: 8
```

---

## What this is

SwarmRT is a runtime + language for writing concurrent programs that compile to a single native binary.

It takes the parts of the BEAM (Erlang/Elixir's VM) that turned out to matter — lightweight processes, lock-free message passing, supervisors, hot reload, distribution — and reimplements them as a ~20K-line C library plus an ahead-of-time compiler that emits native code. No interpreter. No bytecode. No VM warm-up. Each `.sw` file becomes a standalone executable that boots in <10ms and runs at native C speed.

It exists because the same workload BEAM was built for in 1986 — *thousands of long-lived, message-passing, partial-failure-tolerant processes* — is exactly what you need when you're running a swarm of AI agents. SwarmRT is the substrate behind [swarm-code](https://github.com/skyblanket/swarm-code) and a growing pile of agent-driven tools.

The language is called **`sw`** and is designed so an LLM can write it correctly on the first try.

---

## Quickstart (60 seconds)

```bash
git clone https://github.com/skyblanket/swarmrt && cd swarmrt
make swc libswarmrt          # builds the compiler + runtime library
./bin/swc build examples/counter.sw -o counter
./counter
```

That's it. No package manager, no language server install, no VM image. The compiler is one binary, the runtime is one static library, and `cc` is the only external tool.

---

## Why you might care

| If you're… | What SwarmRT gives you |
|---|---|
| **Running AI agents** | First-class actor model so each agent is a process. Selective receive for tool replies. ETS for shared state. HTTP / WebSocket / Chrome DevTools builtins so an agent can call APIs and drive a browser without spawning a Node sidecar. |
| **Building distributed systems** | Erlang-style multi-node TCP distribution. Supervisors with one-for-one / one-for-all / rest-for-one strategies. Hot code reload. Process linking and monitoring. |
| **Writing concurrent programs** | 100K+ lightweight processes per node. ~150ns context switches. Lock-free MPSC mailboxes. No `async`/`await` keyword salad — just `spawn` and `receive`. |
| **Avoiding language overhead** | One binary, no VM, no GC pauses (per-process generational GC means no global stop-the-world), <10ms startup, dependency-free deploy. |

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
    [1, 2, 3, 4, 5] |> each(fn(n) { send(pid, {'square', n, self()}) })
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
- **Loud runtime failures.** `hd([])`, `elem(t, 99)`, `n / 0` panic with `at src/X.sw:N` instead of silently returning `nil`. `expect(value, msg)` is the idiomatic unwrap. `try/catch` for recoverable cases.
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
| **`wsc_*` WebSocket client** | For MCP, streaming APIs, WS-based LLM servers. |
| **`chrome_launch` + CDP** | Drive a real browser without Playwright/Node sidecar. |
| **ETS** | Agent registry, perms cache, conversation memory, todo state. |
| **`supervise(strategy, children)`** | Crash recovery with restart strategies (one-for-one / one-for-all / rest-for-one). |
| **Hot reload** | Upgrade agent code without killing in-flight processes. |
| **`case`** | Tool-call dispatch: `case tool_name { "read" -> ... ; "bash" -> ... ; _ -> ... }`. |

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
| **Hot reload** | Module versioning, swap running code without stopping processes. |
| **Compiler (`swc`)** | `.sw` → AST → C → native binary. Tail-call optimisation, optional XOR-string obfuscation, optional symbol stripping. |

Numbers: process spawn ~100-500ns, context switch ~150ns, message send ~10ns (pointer sharing), 100K+ concurrent processes per node. Full breakdown in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

---

## Compiler CLI

```
swc build <file.sw> [-o <name>]   Compile to native binary
swc emit  <file.sw>               Print generated C to stdout
swc repl                          Interactive REPL (no file needed)
swc test [<file.sw>|<dir>]        Run test_* functions in .sw files

Options for build/emit
  -o <name>     Output binary name
  -O            Optimise (-O2)
  --obfusc      XOR-encode string literals + mangle symbols
  --strip       Strip the symbol table
  --emit-c      Save the .gen.c next to the binary (useful for debugging codegen)
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

Variables persist across lines. Multi-line input continues until brackets balance. The REPL uses a tree-walking interpreter and supports the language core plus the most-used builtins (strings, JSON, maps, formatting, `case`, `try/catch`). The codegen path supports more (HTTP, WebSocket, Chrome, ETS, processes) — for those, write a `.sw` file and `swc build` it.

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
make test-all        # the full C-runtime + sw test suite
```

`test-sw` runs a small but growing set of `tests/sw/test_*.sw` files via the harness in `tests/sw/run_tests.sh`. Add a `test_<topic>.sw` file there and it'll be picked up automatically.

---

## Build

Requires: a C compiler (cc/clang/gcc) and pthreads. Developed and tested on macOS Apple Silicon; Linux support is the intent (pthreads + posix-only APIs) but hasn't been continuously verified.

```bash
make swc libswarmrt              # compiler + runtime library only (you usually want this)
make all                         # everything: compiler, library, examples, demos
make test-sw                     # sw-language tests
make test-all                    # full test suite
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

Stable enough to be the substrate for [swarm-code](https://github.com/skyblanket/swarm-code). New runtime features land regularly — see [CHANGELOG](docs/CHANGELOG.md). Breaking changes are called out in the changelog and the language reference is the source of truth.

---

## License

MIT — built by [Otonomy](https://otonomy.ai).
