# Known issues

Tracked publicly because users will hit them. Each has a repro and a
hypothesis; PRs welcome.

---

## High-process-count crash — arena-slot reuse race (~62k spawn cliff)

**Status:** **Speculative fix landed (R3-C).** Per-scheduler deferred
free — process_destroy stashes the slot+block in `sched->pending_free_*`
and only releases the *previous* iteration's pending to the partition
free list. Slots become reusable after one full scheduler iteration,
which is microseconds longer than any in-flight `sw_context_swap`.
Macros + helper at `src/swarmrt_native.c:process_destroy /
flush_pending_free`. Verification is on the GitHub Actions
ubuntu-24.04 runner (native amd64, where the race fires reliably)
via the `make stress` job in `.github/workflows/linux-quickstart.yml`
— that job no longer has `continue-on-error: true`, so a regression
flips the workflow red.

**If the next round-2-style review hits this on real Linux:** the fix
didn't take, and one of the heavier paths is needed. Both are
documented below.

---

### Background (in case the fix didn't take)

**Reproduces on:** Ubuntu 24.04 x86_64, gcc 13.3 (reviewer's
environment). Cliff between 60k and 65k spawns. Above 65k: program
rarely prints its result line — crash happens in-flight, not at
shutdown.

**Important methodology note:** an earlier commit (`3a5e029`) claimed
"couldn't repro after 50 stress runs in Docker." That conclusion was
**wrong** — Docker Desktop on macOS Apple Silicon runs x86_64
binaries under qemu user-mode emulation, which serialises thread
scheduling aggressively enough to hide most thread-interleaving races
(this is well-known for TSan/race-detector testing too). The valgrind
check was also at N=1k, well below the ~62k race threshold. Neither
setup had a real chance of triggering the bug. Sorry for the
credibility wobble.

**Reviewer's measurements (before R3-C, on native hardware):**

| `n` spawns | success rate |
|---:|:---:|
| 60,000 | 20/20 |
| 65,000 | 3/20 |
| 67,000 | 1/20 |
| 70,000 | 1/20 |
| 80,000 | 1/20 (reviewer); 4/50 (re-test on `3a5e029`) |
| 100,000 | 2/20 |

**Repro recipe** (native Linux x86_64; do NOT use emulated Docker,
WSL on top of Hyper-V translation, or anything else that serialises
thread scheduling):

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

**GDB backtrace** of one crash (4 threads):

| Thread | State |
|---|---|
| 1 (main) | `pthread_cond_wait` on `_sw_done_cond` |
| 2 (sched) | `_int_malloc` → `sw_val_int` → `Bisect_spawn_n` (still spawning) |
| 3 (sched) | **SIGSEGV in `sw_context_swap` at `swarmrt_asm.S:86` (`ret`), unreadable return address** |
| 4 (io_loop) | `epoll_wait` |

Thread 3 dies at `ret` after restoring `rsp` from a target process's
saved context. Thread 2 is concurrently allocating fresh PCBs.

### Working hypothesis

The race is on `proc->ctx`, not on the stack memory itself.
`process_init_arena` (src/swarmrt_native.c:322-345) `memset`s ctx to
zero and writes new values for rsp/rip/etc. `sw_context_swap`
(src/swarmrt_asm.S:73-86) reads ctx fields one at a time into
registers, then `ret`s. If the destroying thread frees the slot,
another thread's `sw_spawn` pops it and starts overwriting ctx
mid-way through a third thread's swap into that same slot, the third
thread gets a torn read.

### Fix paths

1. **Defer the free by one scheduler tick** (the one landed in
   R3-C). Per-scheduler `pending_free_slot/block`. Slots only become
   reusable after a full scheduler iteration. Smallest change.

2. **Per-process in-swap mutex.** `sw_context_swap` locks before
   reading `to`'s ctx, unlocks after. `process_destroy` acquires the
   lock once before returning the slot. Correct but adds a lock to
   every context switch — measurable hot-path cost.

3. **Refcount the slot.** `sw_spawn`/`sw_context_swap` incref the
   target; `process_destroy` waits for refcount = 0. Same cost
   structure as the mutex.

---
