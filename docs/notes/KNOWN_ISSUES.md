# Known issues

Tracked publicly because users will hit them. Each has a repro and a
hypothesis; PRs welcome.

---

## High-process-count crash — arena-slot reuse race (~62k spawn cliff)

**Status:** **R5 fix landed — believed closed; CI is the gate.**
Per-slot generation counter + ctx_lock close the race deterministically.
The previous heuristic attempts (R3-C 1-slot ring, R4-B 64-slot ring)
were the wrong shape: they delayed slot return hoping to outrun the
swap-in window, but the race is event-bounded (context-switch duration),
not allocation-rate bounded. R4-B actually **regressed** 40k/50k
thresholds because spreading the deferral across more slots increased
allocator pressure during spawn-storms (reviewer measured 26%-74% fail
rates at thresholds that used to be 100% green).

The R5 fix:

1. Every process slot has a `_Atomic uint64_t generation` and an
   `sw_spinlock_t ctx_lock`.
2. `process_init_arena` takes `ctx_lock`, bumps `generation`, writes
   the new ctx fields, releases the lock.
3. The scheduler samples `generation` at pick time. Before swapping in,
   `sw_safe_swap_into` takes `ctx_lock`, re-checks `generation`, copies
   `proc->ctx` into a stack-local, releases the lock, then calls the
   new asm `sw_context_swap_from_copy(from, &local_ctx)`.
4. If the generation no longer matches at the second check, the slot
   was reused between pick and swap — the scheduler abandons this
   process and loops to pick another.

After step 3 the asm reads from a caller-owned ctx copy, so a
concurrent `process_init_arena` writing the original `proc->ctx`
cannot tear the snapshot. That's the deterministic version of what the
ring tried to approximate.

The ring is gone — `process_destroy` returns slots immediately. This
also restores the 40k/50k thresholds that R4-B regressed.

Verification gate is on the GitHub Actions ubuntu-24.04 runner (native
amd64, where the race fires reliably) via the `make stress` job in
`.github/workflows/linux-quickstart.yml`. That job enforces the gate
— a red workflow means the fix didn't take.

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

**R4-B regression measurements** (64-slot ring made things worse —
removed in R5):

| n | R2 baseline | R3 (1-slot) | R4 (64-slot) |
|---:|:---:|:---:|:---:|
| 40k | (untested) | (untested) | 43/50 (14% fail) |
| 50k | 20/20 | (untested) | 13/50 (74% fail) |
| 80k | ~5/20 | 9/50 | 8/50 |

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
`process_init_arena` (src/swarmrt_native.c) `memset`s ctx to zero and
writes new values for rsp/rip/etc. `sw_context_swap`
(src/swarmrt_asm.S) reads ctx fields one at a time into registers,
then `ret`s. If the destroying thread frees the slot, another
thread's `sw_spawn` pops it and starts overwriting ctx mid-way
through a third thread's swap into that same slot, the third thread
gets a torn read.

### Fix paths

1. **Defer the free by one scheduler tick** — R3-C, failed (deferral
   too short).

2. **64-slot ring deferral** — R4-B, failed AND regressed lower
   thresholds (40k/50k where the race never used to fire). The race
   is event-bounded (context-switch duration), not bounded by how
   many allocations have happened, so no ring size fixes it
   deterministically.

3. **Per-slot generation counter + ctx_lock with C-side copy** —
   R5, **shipped**. The C wrapper copies ctx into a stack-local under
   the lock, then a new asm variant restores from that copy. The
   read is race-free because the source memory the asm reads from
   isn't shared with the allocator.

4. **Per-process in-swap mutex.** Alternative we did not pick. Would
   make `sw_context_swap` lock before reading `to`'s ctx, unlock
   after; `process_destroy` would acquire once before returning the
   slot. Correct but locks the asm hot path on every context switch
   — measurable overhead vs the R5 approach where the lock is only
   held during the small C copy.

---
