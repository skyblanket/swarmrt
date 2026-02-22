# SwarmRT

A native runtime and compiler for concurrent programs. Lock-free message passing, lightweight processes, and an ahead-of-time compiler that turns `.sw` source files into standalone binaries.

~20,000 lines of C. No dependencies beyond libc and pthreads.

Built by [Otonomy](https://otonomy.ai).

---

## What This Is

SwarmRT is a from-scratch implementation of the actor model with:

- **Lightweight processes** — 100K+ concurrent processes via arena allocation
- **Lock-free mailboxes** — Vyukov MPSC queues, zero-contention send path
- **Work-stealing schedulers** — per-core threads with priority run queues
- **Preemptive scheduling** — reduction counting, cooperative yields
- **Behaviours** — GenServer, Supervisor, Task, GenStateMachine, ETS
- **Ahead-of-time compiler** — `.sw` source to native binary via C codegen

The language is designed for AI agents to read and write. Minimal punctuation, explicit keywords, no indentation sensitivity.

---

## Quick Start

```bash
make swc libswarmrt

# compile a .sw program to a native binary
./bin/swc build examples/counter.sw -o counter
./counter

# or just see the generated C
./bin/swc emit examples/counter.sw
```

---

## The Language

```
module Counter
export [main, counter]

fun counter(n) {
    receive {
        {'increment', by} ->
            counter(n + by)

        {'get', from} ->
            send(from, {'count', n})
            counter(n)

        'stop' ->
            print("stopped at " ++ n)
    }
}

fun main() {
    pid = spawn(counter(0))
    send(pid, {'increment', 5})
    send(pid, {'increment', 3})
    send(pid, {'get', self()})

    receive {
        {'count', n} -> print("count: " ++ n)
    }

    send(pid, 'stop')
}
```

Output:

```
count: 8
stopped at 8
```

---

## Compiler Pipeline

```
counter.sw ──parse──> AST ──codegen──> counter.c ──cc──> counter
                                  │
                            obfuscate (optional)
                            XOR strings + symbol mangle
```

The compiler (`swc`) parses `.sw` source, walks the AST, and emits C code that links against `libswarmrt.a`. The generated binary is a self-contained native executable with the full runtime embedded.

### What the compiler handles

| Feature | Implementation |
|---------|---------------|
| Spawn | Generates trampoline struct + entry function per spawn site |
| Receive | `sw_receive_any()` + if/else chain for pattern matching |
| Send | Builds `sw_val_t`, calls `sw_send_tagged()` directly |
| Tail calls | Self-recursive calls in tail position compile to `goto` |
| Pipe operator | `x \|> f()` rewrites to `f(x)` at codegen time |
| Pattern matching | Structural match on tuples, atoms, ints, strings |
| Obfuscation | XOR-encoded string literals (key `0xa7`) + FNV-1a symbol mangling |

### CLI

```
swc build <file.sw>     Compile to native binary
swc emit  <file.sw>     Print generated C to stdout

Options:
  -o <name>             Output binary name
  -O                    Optimize (-O2)
  --obfusc              XOR strings + mangle symbols
  --strip               Strip symbol table
  --emit-c              Save .gen.c alongside binary
```

---

## Runtime Architecture

### Process Model

Each process is a 2KB arena-allocated slot with its own stack, mailbox, and reduction counter. No malloc on the spawn hot path — processes come from a pre-allocated slab.

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

### Message Passing

Lock-free MPSC (multi-producer, single-consumer) design:

- **Send path**: atomic CAS push to signal stack — no locks, no contention
- **Receive path**: bulk steal from signal stack, reverse to FIFO, scan private queue
- **Tagged messages**: selective receive by tag (call, cast, exit, down, timer, port)

### Behaviours

Built on top of the native runtime:

| Behaviour | What it does |
|-----------|-------------|
| GenServer | Request-reply (`sw_call`) and async cast (`sw_cast`) with state management |
| Supervisor | Child specs, restart strategies (one-for-one, one-for-all, rest-for-one) |
| Task | Async/await with automatic linking |
| GenStateMachine | State machines with event-driven transitions |
| ETS | Concurrent in-memory tables (set, ordered_set, bag) |
| Registry | Named process lookup with O(1) hash table |

### Additional Systems

- **IO/Ports** — kqueue-based async I/O, TCP accept/read/write as port messages
- **Hot code reload** — module versioning, swap running code without stopping processes
- **Generational GC** — per-process heaps with minor/major collection
- **Distribution** — multi-node TCP message routing with automatic reconnection

---

## Build

Requires: a C compiler (cc/clang/gcc) and pthreads. macOS or Linux.

```bash
# everything
make all

# just the compiler + library
make swc libswarmrt

# run all phase tests (83 tests across 10 phases)
make test-phase2 test-phase3 test-phase4 test-phase5 \
     test-phase6 test-phase7 test-phase8 test-phase9 test-phase10

# native runtime benchmark
make test-native
```

### Compile a `.sw` program

```bash
# basic
./bin/swc build examples/counter.sw -o counter

# optimized + obfuscated
./bin/swc build examples/counter.sw -o counter -O --obfusc --strip
```

The compiler finds `libswarmrt.a` and headers relative to its own location. If you move `swc`, set the library/include paths manually:

```bash
cc -O2 -o counter generated.c -I/path/to/src -L/path/to/bin -lswarmrt -pthread
```

---

## Project Structure

```
src/
  swarmrt_native.{c,h}    Core runtime: scheduler, spawn, send/receive, arena
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
  swarmrt_codegen.{c,h}    AST-to-C code generation
  swarmrt_obfusc.c         String XOR encoding + symbol mangling
  swc.c                    Compiler CLI driver

examples/
  counter.sw               Process spawning, send/receive, pattern matching
  pingpong.sw              Bidirectional message passing between processes
  hello.sw                 Minimal program

Makefile                   All build targets
```

---

## Language Reference

### Types

| Type | Syntax | Example |
|------|--------|---------|
| Integer | bare number | `42` |
| Float | number with dot | `3.14` |
| String | double quotes | `"hello"` |
| Atom | single quotes | `'ok'` |
| Tuple | braces | `{'tag', value}` |
| List | brackets | `[1, 2, 3]` |
| Pid | returned by `spawn` | `spawn(f(x))` |
| Nil | keyword | `nil` |

### Operators

```
+  -  *  /          arithmetic
++                   string concatenation
==  !=  <  >  <=  >=  comparison
&&  ||               logical
|>                   pipe (left value becomes first argument)
=                    assignment
```

### Keywords

```
module    declare module name
export    list exported functions
fun       define a function
spawn     create a new process
send      send a message to a process
receive   wait for a message with pattern matching
self      get current process pid
if/else   conditional
after     timeout in receive block
```

### Pattern Matching

Receive clauses match on structure:

```
receive {
    {'ok', value}    -> handle(value)      # match tuple with atom tag
    {'error', reason} -> fail(reason)
    42               -> got_the_answer()   # match literal int
    'done'           -> cleanup()          # match atom
}
```

Variables in patterns bind to the matched value. Atoms (single-quoted) match literally.

### Tail Call Optimization

Self-recursive calls in tail position are compiled to `goto`, so recursive process loops don't grow the stack:

```
fun loop(state) {
    receive {
        {'update', new} -> loop(new)    # compiled to: state = new; goto top
        'stop' -> state
    }
}
```

---

## For AI Agents

The `.sw` syntax was designed for LLM code generation:

- **No indentation sensitivity** — brace-delimited blocks
- **Keywords over symbols** — `spawn` instead of `!`, `receive` instead of `case`
- **Minimal punctuation** — fewer tokens to get wrong
- **Pipe operator** — `data |> transform()` reads naturally
- **Explicit concurrency** — `spawn`, `send`, `receive` are named operations

An agent can write a `.sw` file, compile it with `swc`, and run the resulting binary. The compilation step catches syntax errors at build time rather than runtime.

---

## License

MIT — Copyright 2026 Otonomy
