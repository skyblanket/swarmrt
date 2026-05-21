# Known issues

Tracked publicly because users will hit them. Each has a repro and a
hypothesis; PRs welcome.

---

## SIGSEGV on shutdown after `main()` returns (Linux only)

**Status:** Open. Confirmed on Ubuntu 24.04 x86_64 (gcc 13.3.0).
Does not reproduce on macOS Apple Silicon.

**Repro:** the spawn-100k microbench. ~2 runs in 3 segfault on exit
*after* printing the correct result line.

```sw
module Bench
export [main]

fun child(parent) { send(parent, 'done') ; 'ok' }
fun spawn_n(n) {
    if (n == 0) { 'ok' } else { spawn(child(self())) ; spawn_n(n - 1) }
}
fun await_n(n) {
    if (n == 0) { 'ok' } else { receive { 'done' -> await_n(n - 1) } }
}
fun main() {
    n = 100000
    t0 = timestamp()
    spawn_n(n); await_n(n)
    print(f"spawned + joined {n} procs in {timestamp() - t0} ms")
}
```

Observed:
```
spawned + joined 100000 procs in 864 ms
[SwarmRT] CRASH: SIGSEGV at address (nil)
Backtrace: (empty)
```

User-visible output is correct (the workload completed). Exit code is
non-zero, so CI/test harnesses flag it.

**Hypothesis:** race between the `main()` return path and the scheduler
threads still draining run queues. Sequence:

1. User `main()` returns.
2. `_main_entry` (the SwarmRT process wrapping the user's main) signals
   `_sw_done_cond` and returns to the scheduler that ran it.
3. C `main()` wakes up, calls `sw_shutdown(0)`.
4. `sw_shutdown` sets `sched->should_exit = 1` and pthread_joins.
5. Other schedulers are mid-iteration on processes that, although done,
   haven't been observed as `SW_PROC_EXITING` yet.
6. `sw_shutdown` continues to munmap process stacks — including stacks
   still being used by an in-flight scheduler.

**Empty backtrace** is consistent with this — the process whose stack
was just unmapped can't unwind.

**Suggested investigation path:**
- Run under `valgrind --tool=helgrind` and `valgrind --tool=memcheck`
  against the bench above.
- Before munmap-ing process stacks, add a barrier that waits for
  `proc->state != SW_PROC_RUNNING` for every slot.
- Or: have `sw_shutdown` first set `g_swarm->running = 0`, wait for all
  schedulers' `current` to be NULL, *then* join and free.

**Workaround:** none currently. The output is correct, so silence the
SIGSEGV via `(./prog; true)` in CI scripts if you need a clean exit
code while this is open.

**Why this matters:** undermines the "deterministic native binary"
framing in the README. Most visible to first-time Linux users.
