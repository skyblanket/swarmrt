# SwarmRT Benchmarks

**Hardware:** Apple Silicon (M-series)
**Build:** `cc -O2 -pthread`

---

## Native Runtime Performance

| Metric | Value |
|--------|-------|
| Process spawn | ~100-500ns |
| Context switch | ~100-200ns (assembly) |
| Message send (local) | ~10ns enqueue + payload deep-copy (O(message size); no shared heap) |
| Memory per process | ~2KB PCB + 128KB stack + value arena (8KB initial, grows; freed on exit) |
| Max concurrent processes | 100K+ |

---

## Process Spawn

```
100 processes:     < 1ms
1,000 processes:   ~5ms
10,000 processes:  ~50ms
100,000 processes: ~500ms
```

Arena-allocated PCBs — no malloc on the hot path. Spawn cost is dominated by stack allocation (single mmap per scheduler partition).

---

## Context Switch

Assembly context switching (ARM64 `swarmrt_asm.S`):

```
Register save/restore: ~100ns
Full process switch:   ~150-200ns
```

Saves callee-saved registers only (x19-x28, d8-d15, sp, lr on ARM64). No kernel transition.

---

## Message Passing

Lock-free MPSC queue (Vyukov design):

```
Local send:       ~10ns (atomic CAS push) + payload deep-copy (O(message size))
Selective receive: ~50-100ns (queue scan by tag)
Cross-scheduler:   ~100-200ns (includes cache line transfer)

Note: GC v1 gives each process a value arena freed on exit (no shared heap), so a
send deep-copies its payload onto the global heap (which the receiver then reads) —
the ~10ns is the queue op; total send cost adds an O(message-size) copy. Small
messages stay cheap. (Those global-heap copies are not yet lifecycle-reclaimed —
Ownership v2 moves them to owned, freed regions.)
```

Same-node messages are deep-copied into a global-heap value graph the receiver reads
(GC v1 copy-on-escape: no serialization, but an O(size) copy — not pointer-sharing).
Cross-node messages are marshaled over TCP.

---

## Scheduling

Reduction-counted preemptive scheduling:

```
Time slice:        2000 reductions
Priority levels:   4 (max, high, normal, low)
Work stealing:     idle schedulers steal from busy ones
```

---

## Build & Run

```bash
make test-native    # run the full benchmark suite
make h2h            # head-to-head benchmark
```
