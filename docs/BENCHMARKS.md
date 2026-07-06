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

Note: each process has a value arena (no shared heap), so a send deep-copies its
payload into a message region the receiver ADOPTS on match (Ownership v2) — the ~10ns
is the queue op; total send cost adds an O(message-size) copy. Small messages stay
cheap, and the region is reclaimed in the receiver's lifecycle (no leak).
```

Same-node messages are deep-copied into a per-message region the receiver adopts into
its own arena on match (Ownership v2: no serialization, no shared pointers — an O(size)
copy that is then lifecycle-reclaimed). Cross-node messages are marshaled over TCP.

---

## Scheduling

Reduction-counted preemptive scheduling:

```
Time slice:        2000 reductions
Priority levels:   4 (max, high, normal, low)
Work stealing:     idle schedulers steal from a global overflow queue
```

Note on work stealing: idle schedulers steal from a shared global overflow
queue, not from peers' local run queues. A runnable process sitting in a busy
scheduler's local queue is therefore not stolen — see
[KNOWN_ISSUES.md](notes/KNOWN_ISSUES.md). This is why a strictly sequential
cross-scheduler `pingpong` scales worse at N schedulers than at 1 (the
`bench/actor` suite below shows it); a scheduler-locality fix is in progress.

---

## Build & Run

```bash
make test-native        # native micro-benchmarks (spawn/ctx-switch/send)
./bench/actor/run.sh     # head-to-head vs Erlang/OTP + Go (see below)
```

---

## Cross-runtime comparison (swarmrt vs Erlang/OTP vs Go)

`bench/actor/` runs four identical actor workloads — spawn, pingpong, fanout,
parallel — in `sw`, Erlang, and Go, reporting best-of-3 wall-clock seconds on
the same machine (`erl`/`go` auto-detected and skipped if absent):

```bash
./bench/actor/run.sh                    # default scheduler count
SW_SCHEDULERS=1 ./bench/actor/run.sh    # single-scheduler
```

On an idle Apple Silicon box (OTP 28 BeamAsm, Go 1.26), a single `sw` scheduler
beats Erlang/OTP on spawn, pingpong, and fanout; at default schedulers swarmrt
also beats Erlang on `parallel` and `fanout`. Two multi-scheduler scaling costs
are called out honestly in [bench/actor/README.md](../bench/actor/README.md)
(sequential-pingpong cross-scheduler bounce, spawn contention) and are being
optimized. Go's runtime is the raw-speed leader. Numbers vary by hardware and
machine load — run the harness on an idle box to get yours.
