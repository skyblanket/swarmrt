# SwarmRT Benchmark Results

**Date:** 2026-02-21  
**Hardware:** Mac17,2 (Apple Silicon M1 Pro equivalent)  
**Build:** `gcc -O2 -pthread`

---

## Summary

| Test | Result | Status |
|------|--------|--------|
| Spawn 100 processes | 1.29ms (12.9μs/proc) | ✅ Works |
| Spawn 1,000 processes | 21.8ms (21.8μs/proc) | ✅ Works |
| Spawn 10,000 processes | 598ms (59.8μs/proc) | ✅ Works |
| 1,000 context switches | 1.3s (134μs/switch) | ⚠️ Slow |
| 10,000 context switches | 1.3s (13.4μs/switch) | ⚠️ Slow |
| Process completion | 10,000/10,000 | ✅ 100% |

---

## Detailed Results

### Process Spawn

```
100 processes:    1.29 ms total, 12.92 μs/proc,   77,399 spawns/sec
1,000 processes:  21.84 ms total, 21.84 μs/proc,  45,783 spawns/sec  
10,000 processes: 597.75 ms total, 59.77 μs/proc, 16,729 spawns/sec
```

**Observation:** Spawn time increases with load due to pthread contention. Not as lightweight as BEAM's ~1μs.

### Context Switch

```
1,000 yields:   1,296 ms,  1,296,606 ns/switch
10,000 yields:  1,298 ms,    133,868 ns/switch
```

**Observation:** Using `pthread_yield_np()` is **very slow** (~134μs vs BEAM's ~300ns). This is expected - OS thread yield is expensive.

---

## Comparison: SwarmRT vs BEAM/Elixir

| Metric | BEAM (actual) | SwarmRT (actual) | Ratio |
|--------|---------------|------------------|-------|
| Process spawn | ~1μs | ~13-60μs | **13-60x slower** |
| Context switch | ~300ns | ~134μs | **447x slower** |
| Memory overhead | ~300B | ~66KB | **220x more** |
| Raw compute | Bytecode | Native | **SwarmRT wins** |
| Max processes | Millions | ~10K (memory) | **BEAM wins** |
| Safety | GC'd, isolated | Manual, shared | **BEAM wins** |

---

## The Truth

### What SwarmRT Actually Is

A **pthread wrapper** with BEAM-like semantics, not a true lightweight runtime.

### Why It's Slower

| BEAM | SwarmRT (Current) |
|------|-------------------|
| User-space scheduling | OS pthread scheduling |
| ~1KB stack per process | 64KB+ pthread stack |
| Custom context switch | `setjmp`/`pthread_yield` |
| Copying GC per process | No GC (manual) |
| 30 years optimization | Days old |

### Where SwarmRT Wins

1. **Raw compute** - Native C speed inside processes
2. **FFI** - Direct C calls, no NIF complexity
3. **Simplicity** - 600 lines vs 200K+ lines
4. **AI syntax** - Easier to generate than Elixir

### The Real Use Case

Not a BEAM replacement, but:

```
┌─────────────────────────────────┐
│ BEAM/Elixir (version-ctrl)      │  ← Coordination, supervision
│  - Distributed consensus        │
│  - Hot reloading                │
│  - OTP behaviors                │
├─────────────────────────────────┤
│ Port/NIF                        │
├─────────────────────────────────┤
│ SwarmRT (compute kernel)        │  ← Fast C compute
│  - Parallel algorithms          │
│  - AI inference                 │
│  - Data processing              │
└─────────────────────────────────┘
```

---

## To Match BEAM Performance

Would need to implement:

1. **User-space threads** - Custom stack switching (swapcontext/makecontext or hand-rolled)
2. **Small stacks** - 1KB per process, growable
3. **Reduction counting** - Preemptive scheduling
4. **Copying message passing** - True isolation
5. **Per-process heaps** - GC per actor

This is essentially **rewriting BEAM**. The question: why not just use BEAM?

---

## Honest Assessment

**Verdict:** SwarmRT as currently implemented is a useful learning project and could work as a **compute offload** from BEAM, but it's not a replacement.

**Better approach for AI-friendly language on BEAM:**

```elixir
# Use Elixir macros to create AI-friendly DSL
defmodule SwarmCode do
  defmacro spawn(fun) do
    quote do: spawn(unquote(fun))
  end
  
  defmacro swarm_map(collection, fun) do
    quote do
      unquote(collection)
      |> Enum.map(&Task.async(fn -> unquote(fun).(&1) end))
      |> Enum.map(&Task.await/1)
    end
  end
end
```

Or: **Compile SwarmRT syntax to BEAM bytecode** (like Gleam/LFE do).

---

## Files

- `src/swarmrt_simple.c` - 200-line pthread-based runtime
- `src/parser.c` - 700-line parser for AI syntax
- `src/benchmark.c` - Benchmark suite

**Total:** ~1,400 lines of C, functional but not production-grade.
