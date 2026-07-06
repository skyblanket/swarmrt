# Actor-model benchmarks: swarmrt vs Erlang/OTP vs Go

Four workloads that stress the parts of an actor runtime that actually matter —
cheap process creation, message round-trip latency, one-way throughput, and
multicore scaling — written identically in `sw`, Erlang, and Go.

```
./bench/actor/run.sh                    # default scheduler count (nproc)
SW_SCHEDULERS=1 ./bench/actor/run.sh    # swarmrt single-scheduler
```

Erlang (`erl`/`erlc`) and Go (`go`) are auto-detected and skipped gracefully if
absent, so the swarmrt column always runs. Times are **best-of-3 wall-clock
seconds, lower is better**. This is a *relative* harness: absolute numbers vary
with hardware and machine load — the point is the columns side by side, measured
in the same run on the same box. Run it on an otherwise-idle machine.

## The workloads

| workload | what it does | what it measures |
|---|---|---|
| `spawn` | spawn 1,000,000 processes that immediately exit | process creation + teardown cost |
| `pingpong` | 2 processes exchange 2,000,000 request/reply round-trips | message round-trip latency (fully sequential) |
| `fanout` | 1 producer streams 5,000,000 messages to 1 consumer | one-way send throughput |
| `parallel` | 20 independent producer/consumer pairs, 500,000 msgs each (10M) | multicore message-passing scaling |

## Representative results

Apple Silicon (M-series), otherwise idle, OTP 28 (BeamAsm JIT), Go 1.26. Your
numbers will differ; re-run the harness to get yours.

**swarmrt single-scheduler (`SW_SCHEDULERS=1`):**

| bench | swarmrt | erlang | go |
|---|---|---|---|
| spawn | **0.33** | 1.57 | 0.21 |
| pingpong | **0.68** | 1.77 | 0.35 |
| fanout | **0.76** | 1.49 | 0.33 |

One `sw` scheduler beats Erlang/OTP on all three — spawn/teardown, round-trip
latency, and one-way throughput. Go's runtime remains the fastest here.

**Default schedulers (nproc):**

| bench | swarmrt | erlang | go |
|---|---|---|---|
| spawn | 2.64 | 1.57 | 0.21 |
| pingpong | 4.28 | 1.77 | 0.35 |
| fanout | 1.11 | 1.49 | 0.32 |
| parallel | **0.49** | 1.30 | 0.24 |

swarmrt beats Erlang on `parallel` and `fanout` and scales genuinely-parallel
work well (`parallel` is 3.4× faster than its own single-scheduler time). Two
multi-scheduler costs are visible and honest:

- **`pingpong` is slower at N schedulers than at 1** (4.28 vs 0.68). A strictly
  sequential 2-process round-trip has no parallelism to exploit, so the pair
  bounces between scheduler threads and pays a cross-thread wake per message.
  This is a known scaling characteristic with an in-progress fix (scheduler
  locality / stealable wake routing) that brings it back under the
  single-scheduler number; it is not yet shipped because it must clear the full
  concurrency gate suite first (TSan, the spin-wedge deadlock reproducer, and
  the phase tests) — the scheduler is the runtime's most delicate code.
- **`spawn` contends at N schedulers** (2.64 vs 0.33): a separate allocator /
  process-table contention item.

Takeaway: swarmrt's per-scheduler actor performance already beats Erlang/OTP;
the remaining gaps are multi-scheduler scaling of specific patterns, tracked and
being optimized. Go's runtime is the raw-speed leader across the board.
