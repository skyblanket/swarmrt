# Per-process memory baseline (Phase 0)

The starting point for docs/design/PROCESS_WEIGHT.md. Every later phase
compares against these numbers.

- Commit: `main` @ f9e4929 plus this harness.
- Host: Linux x86-64, a 4 vCPU container, glibc.
- Memory: `bash bench/memory/measure.sh`, with the ring built `-O`,
  `SW_SCHEDULERS=4`. Each figure is the median of the RSS samples at 6, 8 and
  10 s after READY, minus the N=0 run, divided by N.
- Throughput: `bash bench/actor/run.sh` with default schedulers (nproc = 4),
  best of 3.

## Memory

| Mode | N | Per process | RSS samples at 6 / 8 / 10 s | Mappings (VMAs) |
|---|---|---|---|---|
| busy | 10,000 | **161 KB** (161,112 B) | 1,287 / 1,617 / 2,134 MB | 20,091 |
| busy | 30,000 | **93 KB** (92,848 B) | 2,554 / 2,791 / 3,037 MB | 60,105 |
| idle | 10,000 | **10.7 KB** (10,733 B) | 113 MB, flat | 20,063 |
| idle | 30,000 | **10.7 KB** (10,726 B) | 327 MB, flat | 60,062 |

- Busy RSS is **still climbing at 10 s**. Each busy process heads for the
  256 KB turn-reset threshold. At N=30K each process has received fewer
  messages by 10 s, so its figure is lower, not better. A single 11 s sample
  at N=10K (docs/design §2) was 252 KB.
- Each process costs two mappings (its stack plus a guard page). At N=30K the
  program uses 60K of stock Linux's 65,530 (`vm.max_map_count`). Spawning
  about 33K processes crashes (docs/design §2), so the harness stops at 30K
  until Phase 2.
- macOS numbers come from the report-only `Per-process memory` step in the
  `macos` CI job. They will be added here from the first run on `main`.

## Throughput (bench/actor, seconds, lower is better)

| Bench | swarmrt | Go 1.24.7 |
|---|---|---|
| spawn: 1,000,000 processes that exit | 8.31 | 0.55 |
| pingpong: 2,000,000 round trips | 8.92 | 1.07 |
| fanout: 5,000,000 messages, 1 → 1 | 11.25 | 0.96 |
| parallel: 20 pairs × 500,000 messages | 11.64 | 1.56 |

Erlang isn't installed on this host, so its column was skipped. Later phases
must stay within the per-phase budgets in the design doc (≤ 3% for Phase 1).

## After Phase 1

Same host and harness. The default `SW_TURN_RESET_BYTES` is 4 KB.

| Mode | N | Per process | Baseline | RSS samples at 6 / 8 / 10 s |
|---|---|---|---|---|
| busy | 10,000 | **12.5 KB** (12,502 B) | 161 KB, climbing | 130 / 131 / 131 MB, flat |
| busy | 30,000 | **12.1 KB** (12,115 B) | 93 KB, climbing | 369 / 369 / 372 MB, flat |
| idle | 10,000 | **6.4 KB** (6,442 B) | 10.7 KB | 70 MB, flat |

The turn-reset threshold trades memory for throughput (busy, N=10K):

| `SW_TURN_RESET_BYTES` | 2 KB | 4 KB (default) | 8 KB | 16 KB | 256 KB (old) |
|---|---|---|---|---|---|
| Per process | 10.1 KB | 12.6 KB | 17.0 KB | 29.8 KB | 193 KB, climbing |

`bench/actor` (seconds, best of 3):

| Bench | Baseline | Phase 1 at 4 KB | Phase 1 at 2 KB |
|---|---|---|---|
| spawn | 8.31 | 7.45 | 7.85 |
| pingpong | 8.92 | 6.39 | 7.88 |
| fanout | 11.25 | 5.73 | 6.05 |
| parallel | 11.64 | 4.74 | 5.43 |

`MALLOC_ARENA_MAX=1` saves 5.8% more (11.8 KB), under the plan's 10% bar, so it
isn't set.

## After Phase 2a (carriers, every parked process pins)

Same host and harness. Every process in the ring is parked in receive, so each
one pins a carrier. Memory stays comparable to Phase 1 (about 0.6 KB more) and mappings match it, by design; 2a only moves
the stacks into a pool. The drop comes once parked processes stop pinning.

| Mode | N | Per process | Phase 1 | Mappings (VMAs) |
|---|---|---|---|---|
| busy | 10,000 | 13.3 KB | 12.5 KB | 20,063 |
| busy | 30,000 | 12.8 KB | 12.1 KB | 60,063 |
| idle | 10,000 | 7.0 KB | 6.4 KB | 20,062 |
| idle | 30,000 | 7.0 KB | — | 60,062 |

`bench/actor`, main and 2a run back to back (seconds, best of 3):

| Bench | main (Phase 1) | Phase 2a |
|---|---|---|
| spawn | 6.83 | 7.26 |
| pingpong | 9.63 | 8.00 |
| fanout | 5.46 | 5.01 |
| parallel | 4.85 | 4.93 |

## After Phase 2c (compiled processes park stacklessly)

Same host and harness. N=99,000 is the largest ring under the build's 100K process
maximum (the ring plus its root).

| Mode | N | Per process | After 2a | Mappings (VMAs) |
|---|---|---|---|---|
| busy | 10,000 | 8.9 KB | 13.3 KB | 72 |
| busy | 99,000 | **8.4 KB** | — (needed 198K mappings) | 83 |
| idle | 10,000 | 2.6 KB | 7.0 KB | 72 |
| idle | 99,000 | **2.5 KB** | — | 74 |

Busy processes still carry up to `SW_TURN_RESET_BYTES` of dead values between turn
checkpoints: at N=99,000, `1k` gives 4.86 KB and `2k` 6.23 KB per process. Compaction
at park (Phase 2f) removes that garbage at the default.

`bench/actor` (seconds, best of 3), 2c runtime only vs this phase, back to back:

| Bench | 2c runtime | 2c codegen |
|---|---|---|
| spawn | 7.58 | 7.31 |
| pingpong | 8.17 | 8.72 |
| fanout | 5.36 | 5.39 |
| parallel | 4.64 | 4.82 |

## After Phase 2e (builtins that wait park stacklessly)

`bench/memory/task_stream.sh`: N `Std.task_stream` workers that all wait, sampled 8 s
in. The per-process cost is the slope between N=1,000 and N=10,000. Workers in `llm`
mode wait on `llm_complete` against `bench/rollout`'s mock server, which never
answers. Each run has 256 offload worker threads, a fixed cost.

| Mode | Before (Phase 2d) | After 2e | Mappings at N=10,000 |
|---|---|---|---|
| idle (`receive`) | 5.5 KB | 5.6 KB | 76 → 75 |
| llm (`llm_complete`) | **27.5 KB** | **6.6 KB** | 20,623 → 642 |

An idle `task_stream` worker costs more than an idle ring process (2.5 KB). It also
carries its closure and captures in a spawn region, the coordinator's bookkeeping for
it, and the per-process trace block (about 410 B since Phase 1 cut it to 16 entries).
Phases 2f (compaction at park) and 2g (trace state on the carrier) target that, and
with it the plan's under-5 KB goal for this run.

## After Phase 2f (compaction at park)

`bench/memory` ring at N=99,000, back to back with Phase 2e (two runs each):

| Mode | After 2e | After 2f |
|---|---|---|
| busy | 8.3–8.6 KB | **4.8–4.9 KB** |
| idle | 2.6 KB | 2.6 KB |

The default trigger (`SW_PARK_COMPACT_MIN` 2 KB) is what gets busy processes under 5 KB:
at 4 KB they cost 7.8 KB, at 8 KB 8.1 KB.

`bench/memory/ring_throughput.sw` (`-O`, every process passes a token, two runs each).
Each worker counts its own hops; the count is taken once the tokens are out and again
5 s later.

| Ring | 2e hops in 5 s | 2f hops in 5 s |
|---|---|---|
| 1,000 | 6.5–7.1M | 7.1–8.1M |
| 20,000 | 13.3–15.3M | 13.0–14.9M |
| 99,000 | 37–45M | **52.6–53.5M** |

One cost stays open. At 99,000, `main` takes 12.7–15.2 s instead of 8.3–8.5 s to send
the tokens into the already-running ring, and the busy `measure.sh` ring takes 29 s
instead of 13 s to reach READY. Spawning (0.9 s against 1.0 s) and the `go` broadcast
(3.3 s both) are unchanged. Phase 2g found the cause and recovers most of it (below).

`task_stream.sh`: idle 5.9 KB, `llm_complete` 7.1 KB per waiting worker, within noise of
2e. Those workers' arenas stay under the trigger, so compaction doesn't apply to them.

## After Phase 2g (trace state on the carrier)

`bench/memory` ring at N=99,000, back to back with Phase 2f (two runs each):

| Mode | After 2f | After 2g |
|---|---|---|
| busy | 4.9 KB | **4.3 KB** |
| idle | 2.6 KB | **2.2 KB** |
| busy ring, time to READY | 28.2–28.6 s | 15.0–18.7 s (2e: 13.7 s) |

Idle drops by the per-process trace block (`sw_gen_exec_t`, about 410 B plus its malloc
header), which now lives on the carrier. Busy drops by that and by the arena change
below.

`bench/memory/ring_throughput.sw` (`-O`, two runs each):

| Ring | 2f hops in 5 s | 2g hops in 5 s |
|---|---|---|
| 1,000 | 7.4M | 7.7–8.0M |
| 20,000 | 12.4–12.6M | 13.7–14.3M |
| 99,000 | 49.5–50.0M | 52.7–55.4M |

`task_stream.sh`, per waiting worker from the 1,000 → 10,000 slope: `llm_complete` 7.1 →
6.6 KB, idle about 450 B lower at N=10,000.

**The 2f distribution cost.** While `main` sends tokens into a running 99K ring, three of
the four schedulers sit idle and the ring runs on `main`'s thread, so whatever a hop
costs comes out of `main`'s sends. Instrumented, a ring worker compacted on every third
park (1.9M compactions, 13.8 s of CPU, about 7 µs each), copying about 80 B and freeing
about 2.6 KB spread over 27 chunks. The cost was walking and freeing those cold chunks.
Each received message is its own small chunk, and adopting one into the arena made it
the head, so the process's next allocation started yet another small chunk.

`sw_varena_adopt` now puts a message's chunks behind the head whenever the head
carries no mark (it grew since the last `sw_varena_mark`), so the process keeps filling
its own chunk; a head that may be a live mark's chunk keeps the old order. That cut
the chunks per compaction from 27 to about 17. Time for `main` to send the tokens
(medians of four runs, `ring_dist`-style timing, not `-O`): 2e 9.4 s, 2f 14.9 s, 2g
10.6 s; 2g with compaction off 9.1 s. Raising `SW_PARK_COMPACT_MIN` to 4 KB removes the
rest but puts busy processes at 7.4 KB, so the default stays at 2 KB.

What's left is about one cold chunk walked per message received. Copying small messages
into the receiver's arena on receive would remove it. So would Phase 4's allocator.

