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
