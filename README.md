# 🐝 SwarmRT - Minimal BEAM-alike Runtime in C

A lightweight, swarm-native runtime inspired by the BEAM. Built for AI-agent coordination.

**Status:** Core runtime working, parser functional, runtime execution in progress.

---

## Core Philosophy

- **Processes are cheap** — create millions of them
- **Share nothing** — message passing only  
- **Let it crash** — supervisors handle failures
- **Swarm-first** — parallelism is a language primitive

---

## Architecture

```
┌─────────────────────────────────────────┐
│           Swarm Scheduler               │
│  (Distributes tasks across schedulers)  │
└─────────────────────────────────────────┘
                    │
    ┌───────────────┼───────────────┐
    ▼               ▼               ▼
┌─────────┐    ┌─────────┐    ┌─────────┐
│Scheduler│    │Scheduler│    │Scheduler│
│   0     │    │   1     │    │   N     │
│(thread) │    │(thread) │    │(thread) │
└────┬────┘    └────┬────┘    └────┬────┘
     │              │              │
  Run Queue     Run Queue      Run Queue
  ┌─┬─┬─┐       ┌─┬─┬─┐        ┌─┬─┬─┐
  │P│P│P│       │P│P│P│        │P│P│P│
  └─┴─┴─┘       └─┴─┴─┘        └─┴─┴─┘
```

---

## AI-Friendly Syntax

```erlang
# Simple Hello World
module Hello

export [main]

fun main() {
    print("Hello, Swarm!")
}

# Parallel operations - swarm primitives
module DataProcessor

export [analyze_files]

fun analyze_files(paths) {
    # Parallel map - spawns process per item automatically
    results = swarm map(analyze_file, paths)
    
    # Or with pipe syntax (AI-friendly!)
    results = paths |> swarm map(analyze_file)
    
    # Supervisor handles crashes
    supervise results with [retry_3x, fallback_to_cache]
}

# Actor pattern
fun counter(start) {
    receive {
        {increment, by} -> 
            counter(start + by)
        
        {get, from} -> 
            send(from, {count, start})
            counter(start)
        
        stop -> 
            print("Counter stopped")
    }
}
```

---

## Build & Run

```bash
# Build everything
make

# Run tests
./bin/swarmrt test

# Parse a .sw file
./bin/swarmrt parse examples/hello.sw

# Stats
make stats
```

---

## What's Implemented

| Feature | Status |
|---------|--------|
| Multi-threaded schedulers | ✅ |
| Process spawning | ✅ |
| Message queues | ✅ |
| Context switching | 🔄 (basic) |
| Supervision trees | 🏗️ |
| Module parser | ✅ |
| Function parser | ✅ |
| Swarm primitives | 🏗️ |
| Code generation | 🏗️ |

---

## Why This Syntax?

**Designed for AI code generation:**

1. **Minimal punctuation** — fewer tokens to hallucinate
2. **Explicit braces** — no indentation sensitivity
3. **Keywords over symbols** — `spawn` not `!`, `receive` not `case`
4. **Swarm primitives** — parallel execution as first-class
5. **Pipe operator** — `data |> transform` is AI-friendly

---

## Integration with version-ctrl

Since you already built `version-ctrl` on Elixir/OTP, SwarmRT can:

1. **Compile to BEAM bytecode** — leverage your existing infrastructure
2. **Use version-ctrl as package manager** — `vc install swarmrt`
3. **Share supervision trees** — cross-language fault tolerance
4. **Unified swarm coordinator** — version-ctrl's swarm coordinator manages both Elixir and SwarmRT processes

---

## Next Steps

1. ✅ Core runtime (schedulers, processes, messages)
2. ✅ Parser for AI-friendly syntax  
3. 🔄 Bytecode compiler
4. 🔄 BEAM interoperability
5. 🔄 Hot code reloading
6. 🔄 Distributed processes

---

**Built with 🐝 by AI agents, for AI agents.**
