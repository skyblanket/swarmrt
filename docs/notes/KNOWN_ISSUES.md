# Known issues

Tracked publicly because users will hit them. Each has a repro and a
hypothesis; PRs welcome.

---

## High-process-count crash — arena-slot reuse race (~62k spawn cliff)

**Status:** Open. Confirmed on native Linux x86_64 hardware (Ubuntu
24.04, gcc 13.3). Reviewer reports ~4/50 completions at N=80,000 on
current `main`. Race is unchanged across the recent codegen + panic
fixes — those touch other code paths.

**Important methodology note:** an earlier commit (`3a5e029`) claimed
"couldn't repro after 50 stress runs in Docker." That conclusion was
**wrong** — Docker Desktop on macOS Apple Silicon runs x86_64 binaries
under qemu user-mode emulation, which serializes thread scheduling
aggressively enough to hide most thread-interleaving races (this is
well-known for TSan/race-detector testing too). The valgrind check
was also at N=1k, well below the ~62k race threshold. Neither setup
had a real chance of triggering the bug. The KNOWN_ISSUES entry has
been restored. Sorry for the credibility wobble.

**Success rate (20 runs per n, real Linux hardware):**

| `n` spawns | success rate |
|---:|:---:|
| 60,000 | 20/20 |
| 65,000 | 3/20 |
| 67,000 | 1/20 |
| 70,000 | 1/20 |
| 80,000 | 1/20 (reviewer); 4/50 (re-test on `3a5e029`) |
| 100,000 | 2/20 |

Cliff is sharp between 60k and 65k. Above 65k: the program rarely
prints its `ok N` result line — the crash happens **in flight**, not
at shutdown. Output-by-exit-code check misses this (the SIGSEGV
handler always exits non-zero, but the workload's stdout never
arrives).

**Repro recipe that actually works** (native Linux x86_64; do NOT use
emulated Docker, WSL on top of Hyper-V translation, or anything else
that serializes thread scheduling):

```bash
cat > /tmp/bn.sw << 'EOF'
module Bisect
export [main]
fun child(parent) { send(parent, 'done') ; 'ok' }
fun spawn_n(n) {
    if (n == 0) { 'ok' }
    else { spawn(child(self())) ; spawn_n(n - 1) }
}
fun await_n(n) {
    if (n == 0) { 'ok' }
    else { receive { 'done' -> await_n(n - 1) } }
}
fun main() {
    n = 80000
    spawn_n(n); await_n(n)
    print(f"ok {n}")
}
EOF

./bin/swc build /tmp/bn.sw -o /tmp/bn

complete=0
for i in $(seq 1 50); do
    /tmp/bn 2>/dev/null | grep -q "^ok 80000" && complete=$((complete+1))
done
echo "$complete / 50 completed"
```

On affected hardware: expect 0-10% completions. Anything noticeably
higher means qemu/emulation is suppressing the race — re-check that
you're on a native amd64 host.

**GDB backtrace** of one crash (4 threads):

| Thread | State |
|---|---|
| 1 (main) | `pthread_cond_wait` on `_sw_done_cond` |
| 2 (sched) | `_int_malloc` → `sw_val_int` → `Bisect_spawn_n` (still spawning) |
| 3 (sched) | **SIGSEGV in `sw_context_swap` at `swarmrt_asm.S:86` (`ret`), unreadable return address** |
| 4 (io_loop) | `epoll_wait` |

Thread 3 dies at `ret` after restoring `rsp` from a target process's
saved context. Thread 2 is concurrently allocating fresh PCBs. Reads
as: arena slot was returned to the free list while Thread 3 was still
mid-context-swap into it; Thread 2 picked the freed slot via sw_spawn
and reinitialised its `ctx` (rsp, rip, …) under Thread 3's feet.

**Why this matters:** the README claims "100K+ concurrent processes
per node." That claim is empirically false on native Linux x86_64
above ~62k.

**Working hypothesis** (from reading the code, *not* verified against a
real failing run): the race is on `proc->ctx`, not on the stack memory
itself. `process_init_arena` (src/swarmrt_native.c:322-345) `memset`s
ctx to zero and writes new values for rsp/rip/etc. `sw_context_swap`
(src/swarmrt_asm.S:73-86) reads ctx fields one at a time into
registers, then `ret`s. If the destroying thread frees the slot,
another thread's `sw_spawn` pops it and starts overwriting ctx mid-way
through a *third* thread's swap into that same slot, the third thread
gets a torn read — old rbx / new rsp, for instance. `ret` then jumps
to whatever the stale rsp points to.

Schedulers don't share runqs directly, but the slot lifecycle is:
1. Scheduler A destroys proc → returns slot to partition free list.
2. Scheduler B's sw_spawn pops the slot → process_init_arena rewrites ctx.
3. Some scheduler C may still be reading the old ctx if its swap
   started in (1)'s window.

**Suggested fix paths** (none yet attempted):

1. **Defer the free by one scheduler tick.** Add `pending_free_slot`
   to `sw_scheduler_t`; `process_destroy` pushes the previous
   pending to the partition free list and sets the new pending.
   Slots only become reusable after a full scheduler iteration —
   long enough for any in-flight swap (microseconds) to complete.
   Smallest code change; testable in isolation.

2. **Per-process in-swap mutex.** `sw_context_swap` locks before
   reading `to`'s ctx, unlocks after. `process_destroy` acquires the
   lock once before returning the slot. Correct but adds a lock to
   every context switch — measurable hot-path cost.

3. **Refcount the slot.** `sw_spawn`/sw_context_swap incref the
   target; `process_destroy` waits for refcount = 0. Same cost
   structure as the mutex, slightly different ergonomics.

Whoever takes this on: please verify on native Linux x86_64 against
the `make stress` job and post a before/after run count.

**CI guard:** `make stress` runs the 80k bench 20 times and asserts
≥18 successful `ok 80000` lines. The Linux quickstart workflow
(`.github/workflows/linux-quickstart.yml`) calls this on every push —
GitHub Actions ubuntu-24.04 runners are native amd64 so the race
will fire there if it regresses. **Currently expected to fail this
job** — the entry in KNOWN_ISSUES is the receipt for that until the
fix lands.

**Tracking:** R2-#4 in the round-2 review.
