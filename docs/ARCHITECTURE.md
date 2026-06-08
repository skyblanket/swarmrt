# SwarmRT Architecture

## Overview

SwarmRT is a native actor-model runtime written in C. It provides lightweight processes, lock-free message passing, work-stealing schedulers, and a compiler that translates `.sw` source files to native binaries.

---

## Runtime Layers

```
┌─────────────────────────────────────────┐
│           .sw Language Layer            │
│  Lexer → Parser → AST → C Codegen      │
├─────────────────────────────────────────┤
│         Behaviour Layer                 │
│  GenServer, Supervisor, Task, ETS       │
│  GenStateMachine, Registry              │
├─────────────────────────────────────────┤
│         Native Runtime                  │
│  Scheduler, Spawn, Send/Receive         │
│  Links, Monitors, Timers, Signals       │
├─────────────────────────────────────────┤
│         Infrastructure                  │
│  Arena Allocator, GC, Module Registry   │
│  IO/Ports, Distribution, Context Switch │
└─────────────────────────────────────────┘
```

---

## Process Model

Each process is a 2KB arena-allocated slot:

```
┌──────────────────────────────┐
│     sw_process_t (2KB)       │
├──────────────────────────────┤
│ pid          uint64_t        │
│ state        RUNNING/WAIT/..  │
│ mailbox      MPSC queue      │
│ stack        64KB mmap       │
│ reductions   int32_t         │
│ links        linked list     │
│ monitors     linked list     │
│ timer        optional        │
│ context      saved registers │
└──────────────────────────────┘
```

Processes come from a pre-allocated slab — no malloc on the spawn hot path.

---

## Scheduler Architecture

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

- One OS thread per scheduler
- Each scheduler has its own run queue with 4 priority levels
- Work stealing: idle schedulers steal from busy ones
- Reduction counting: 2000 reductions per time slice

---

## Message Passing

Lock-free MPSC (multi-producer, single-consumer):

1. **Send**: atomic CAS push to signal stack — no locks
2. **Receive**: bulk steal from signal stack, reverse to FIFO
3. **Selective receive**: scan queue by tag (call, cast, exit, down, timer, port)

Messages are tagged with a type for selective receive:

| Tag | Purpose |
|-----|---------|
| `SW_TAG_CALL` | Synchronous request-reply |
| `SW_TAG_CAST` | Async fire-and-forget |
| `SW_TAG_EXIT` | Process exit signal |
| `SW_TAG_DOWN` | Monitor notification |
| `SW_TAG_TIMER` | Timer expiration |
| `SW_TAG_PORT` | IO port message |

---

## Behaviours

| Behaviour | What it does |
|-----------|-------------|
| GenServer | Request-reply (`sw_call`) and async cast (`sw_cast`) with state management |
| Supervisor | Child specs, restart strategies (one-for-one, one-for-all, rest-for-one) |
| Task | Async/await with automatic linking |
| GenStateMachine | State machines with event-driven transitions |
| ETS | Concurrent in-memory tables (set, ordered_set, bag) |
| Registry | Named process lookup with O(1) hash table |

---

## Compiler Pipeline

```
counter.sw ──parse──> AST ──codegen──> counter.c ──cc──> counter
                                  │
                            obfuscate (optional)
                            XOR strings + symbol mangle
```

The compiler emits C code that calls the runtime API directly:
- `sw_spawn()` for process creation
- `sw_send_tagged()` for message sending
- `sw_receive_any()` for blocking receive
- `sw_val_*()` constructors for the value system

---

## Performance Characteristics

| Metric | Value |
|--------|-------|
| Process spawn | ~100-500ns |
| Context switch | ~100-200ns |
| Message send | ~10ns enqueue + payload deep-copy (O(message size), no shared heap) |
| Memory per process | ~2KB PCB + 128KB stack + value arena (8KB initial, grows; freed on exit) |
| Max processes | 100K+ (configurable) |
| Compute | native C speed |

---

## Additional Systems

- **IO/Ports** — kqueue-based async I/O, TCP accept/read/write as port messages
- **Module registry** — versioned C-module slots (`sw_module_register` / `sw_module_upgrade`); a C embedder can re-point a named slot to another C function compiled into the same binary and roll back, notifying tracked processes via `SW_TAG_CODE_CHANGE`. C-API only — no `sw`-level builtin and no loading of new code at runtime (`swc build` AOT-compiles every sw function to a fixed C symbol; there is no `dlopen`/bytecode loader).
- **GC v1 (`swarmrt_varena.c`)** — per-process value arena, freed wholesale when the process exits. Values that escape a process (sent messages, spawn captures, ETS entries, supervisor/timer closures) are deep-copied to the global heap at the boundary (no shared heap, BEAM-style). So spawn-a-worker-per-task reclaims; a single long-lived loop accumulates until it exits (mid-life loop-reset is the next step). The older `swarmrt_gc.c` scaffolding is unused.
- **Distribution** — multi-node TCP message routing with automatic reconnection
- **Context switching** — ARM64 assembly for register save/restore (~100ns)
- **Deadlock watchdog** — background thread (wakes every `SW_DEADLOCK_MS`, default 5000) that detects when every live non-scheduler process is blocked in `receive` with an empty mailbox and no port that could wake it; it logs a one-line `possible deadlock` warning to stderr and is **warn-only — it does not terminate processes**. Disable with `SW_DEADLOCK_DETECT=0`.
