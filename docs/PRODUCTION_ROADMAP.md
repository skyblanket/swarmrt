# SwarmRT — Production Roadmap & Handoff

Status as of HEAD `1a4324a` (2026-06-10). This is the live plan for taking the runtime to a
credible 1.0. **Phase 1 is complete; Phase 2 is next.** Read this before picking up work — it
captures what shipped, what's pending, the gate philosophy, and the landmines.

> Companion docs: `docs/CHANGELOG.md` (what each commit did, newest first), `docs/ARCHITECTURE.md`,
> `docs/BENCHMARKS.md`. The memory-ownership design + ground truth lives in the commit messages
> and the `gc-stress`/`gc-slope` probes under `tests/gc/`.

---

## Exit criteria for "production candidate"

1. Process isolation proven across **every** context switch.
2. **All** long-lived ownership paths have bounded memory.
3. Untrusted input cannot crash the node (or another process).
4. Multi-platform CI + sanitizers + fuzzing + 24h soak stay green.
5. Operators can observe, limit, drain, and recover a running deployment.

---

## Phase status

| Phase | Title | Status |
|------:|-------|--------|
| 1 | Close correctness blockers (ownership + isolation) | ✅ **DONE** (audit-confirmed) |
| 2 | Runtime hardening (race detection, atomics, alloc-failure, soak) | ⏳ **NEXT** |
| 3 | Security & fault isolation (fuzzing, limits) | ⬜ pending |
| 4 | Operational readiness (observability, graceful shutdown, docs) | ⬜ pending |
| 5 | Release discipline (multi-platform CI, gates, semver, independent review) | ⬜ pending |

---

## How to verify (gate philosophy — read this, it's hard-won)

Run these locally (macOS dev) before any commit that touches the runtime:

```
make swc libswarmrt        # must be ZERO warnings (the "no unexplained warnings" gate)
make gc-stress             # ASAN + -DSW_ARENA_POISON: UAF / double-free tripwires (9 gates)
SW_SCHEDULERS=1 make gc-stress   # deterministic single-scheduler interleave
make gc-slope              # memory-slope gates: every long-lived owner must hold FLAT RSS (10 probes)
make test-sw               # 53 sw files / 475 assertions (compiled + interpreter parity)
for p in 2 3 4 5 6 7 8 9 10; do make phase$p && ./bin/test-phase$p; done   # runtime/OTP tests
```

**Lessons that cost us real bugs — do not repeat:**

- **An RSS-slope leak gate is only real if the per-iteration leaked object is big enough to
  clear the noise floor.** A ~1 KB/iteration leak PASSED a 6–10 MB-budget slope (noise masked
  it) — the gate was a no-op. Size the captured object to ~64 KB (`big_string("c", 4096)`), and
  prove the gate **bidirectionally**: neuter the fix, confirm the slope FAILs; restore, confirm
  it PASSes. A leak gate you haven't seen FAIL is not a gate.
- **macOS Apple-clang ASAN has NO LeakSanitizer**, and the gates run `detect_leaks=0`. So ASAN
  here only catches UAF/double-free, **never leaks**. → Phase 2 item: add a **Linux CI leg with
  real LSan** (`detect_leaks=1`). Until then, prefer instrumented alloc/free counters or
  amplified slopes for leak verification.
- **`scheduler_loop` checks `kill_flag` BEFORE `sw_safe_swap_into`.** A fiber killed while parked
  in `sw_receive_any` is torn down by `process_destroy` **without ever resuming**. So any cleanup
  placed in a fiber's *post-receive tail* is dead code on the kill path (the dominant teardown
  path — `exit_proc`/kill is the only teardown verb sw exposes for a bare timer/supervisor).
  → Cleanup that must survive a kill goes in **`proc->on_destroy`** (process_destroy always fires
  it, snapshot-then-clear so it can't double-fire).
- **Arm `on_destroy` BEFORE the child is runnable**, via `sw_spawn_dtor`/`sw_spawn_link_dtor`
  (which thread it through `sw_spawn_opts` ahead of `sw_add_to_runq`). Arming it as the first
  statement of the entry fn leaks on a **pre-trampoline kill** (child killed before it ever runs);
  arming it *after* `sw_spawn` returns is a write-after-free (the slot may already be recycled).
- A **panic** `sw_context_swap`s to the scheduler and **never returns** through the C entry fn —
  so a fiber-tail free is skipped on crash too. Same fix: `on_destroy`.

---

## Phase 1 — DONE (what shipped, 8 commits f9ec360..1a4324a)

Generated-code process isolation is proven and every long-lived ownership path is bounded on
**all** paths including kill and pre-trampoline-kill. Each fix has a bidirectional gate.

| Area | Commit | Fix | Gate |
|------|--------|-----|------|
| `_sw_error` per-process | `f9ec360` | cross-process catch UAF closed (per-process `gen_error` slot) | `tests/gc/error_xproc_repro.sw` (gc-stress, SW_SCHEDULERS=1) |
| trace/line/file per-process | `b6c636b` | panic traces belong to the right process (`sw_gen_exec_t` + `_sw_gen` swapped per context switch) | `tests/gc/trace_xproc_repro.sw` + `scripts/gc_trace_check.sh` |
| ETS bounded | `17d5da2` | free-on-replace/delete/take + **copy-out-on-read** (BEAM semantics) — closed a cross-process aliasing UAF | `slope_ets.sw` + `ets_alias_repro.sw` |
| timer delay/interval | `6f54390`,`d6ae6a0` | cancellable + non-blocking (`usleep`→yielding `sw_receive_any`); closures reclaimed | `slope_interval.sw`, `slope_timer.sw` |
| supervisor child closures | `1ea729c` | master + per-incarnation copy; crash-safe via `on_destroy` | `slope_dynsup_churn.sw`, `sup_restart_repro.sw` |
| kill-path leaks | `dfa8cee` | timer + supervisor reclaim via `on_destroy` (kill bypasses the fiber) | `slope_sup_kill.sw` (+ amplified `slope_interval`) |
| pre-trampoline kill | `1a4324a` | `sw_spawn_dtor` arms `on_destroy` before runnable | `slope_interval.sw` (no-yield kill, both scheduler modes) |

**Accepted-minor residuals (documented, not blockers):**
- A spurious **compound** message sent to a *timer* pid leaks its nested allocations: the timer
  uses `free(m)`, not `_sw_free_global_val(m)` — the latter would **crash** on a RAW (non-value)
  payload returned by `sw_receive_any`. (Timers realistically only get value sends.)
- The C-API `sw_ets_*` in `swarmrt_ets.c` (interpreter/phase-test path, short-lived) still
  leaks-on-replace/delete — out of the compiled sw-language scope this phase covered.
- The caller-side `specs[]` array in `_builtin_supervise` is a one-time ~N×struct leak (an async
  init-race makes freeing it harder); not churn.

**Verification gap to close in Phase 5:** the **ETS** area was gated (`ets_alias_repro` +
`slope_ets`) and self-verified, but the independent fresh-eyes auditor agent failed to report in
**both** audit runs — so ETS has not had a clean independent adversarial pass. Re-audit it.

---

## Phase 2 — RUNTIME HARDENING (NEXT — start here)

Do these in roughly this order. Each is "verify locally, then branch → ff-merge to main → push".

### 2.1 Linux LeakSanitizer CI leg  *(highest leverage — closes the leak-blindness above)*
- Add a job/leg to `.github/workflows/linux-quickstart.yml` that builds the `gc-stress`/`gc-slope`
  probes on Linux and runs them with `ASAN_OPTIONS=detect_leaks=1` (real LSan, which macOS lacks).
  This would have caught the kill-path leaks that passed a green slope. Cannot be verified on the
  macOS dev box — keep the config minimal and low-risk; gate it as advisory first, promote to
  blocking once green.
- Bonus: a small standalone C harness (mirror `gc_ets_alias`) that spins the runtime, runs
  create→cancel/kill→reap loops for timers + supervisors, `sw_swarm_shutdown()`, and lets LSan
  assert **zero leaks at exit** — a precise, non-noisy leak gate.

### 2.2 Scheduler-count matrix  *(locally verifiable)*
- Run `make gc-stress`, `make test-sw`, and the phase tests under `SW_SCHEDULERS=1`, `2`, `$(nproc)`,
  and oversubscribed (`2×nproc`). Fix anything that breaks; wire the matrix into CI.

### 2.3 ThreadSanitizer build + race test
- macOS clang has TSan. Build the runtime under `-fsanitize=thread` and run a multi-scheduler
  spawn/message/kill + supervisor-crash storm. **Caveat:** TSan may false-positive on the custom
  asm context-switch (`swarmrt_asm.S`) the way ASAN does on the fiber stacks — characterize and
  suppress the coroutine noise, keep the real-race signal.

### 2.4 Make shared state atomic-or-locked
- Audit and fix races on: **kill flags**, **process lifecycle state**, **registry + monitor
  references**, **timer cancellation**, **mailbox wakeups**. (The kill path interacts with all of
  these — see the kill_flag-before-swap note above.) Each fix needs a TSan-clean repro.

### 2.5 Allocation-failure safety
- Inject deterministic `malloc`/arena-create failures. Ensure **every** path either returns an
  error or terminates **only the affected process** — and that a partial region-ownership transfer
  (e.g. spawn region adopted but child died) never leaks or double-frees. (See the
  `g_pending_spawn_region` / `g_pending_on_destroy` / `spawn_region` handoff in `sw_spawn_opts`.)

### 2.6 24-hour soak
- Build a mixed-workload harness: actor spawn/message/pmap + supervisor crash/restart storms +
  timers + networking + ETS churn + distribution + a production-like agent workload. Run a short
  version locally now; schedule the full 24h run (cron/wake) and assert bounded RSS + zero
  sanitizer hits + no leaked process slots (phase2 prints `Slots N/N free` — must stay full).

---

## Phase 3 — SECURITY & FAULT ISOLATION
- Fuzz every external-input boundary: parser/compiler, JSON, distribution protocol, HTTP/WebSocket,
  node unmarshalling, database + file builtins. (`make fuzz` already covers parse/marshal/http —
  extend it.)
- Harden limits: max message size, mailbox limits + backpressure, process + memory quotas,
  recursion/decode depth (JSON depth guard `g_json_depth` exists — audit the rest), connection +
  request timeouts.
- Prove malformed input cannot crash the runtime or another process.

## Phase 4 — OPERATIONAL READINESS
- Observability: per-process memory / mailbox depth / reductions, spawn/crash/restart counters,
  scheduler utilization, structured logs + crash reports, health/readiness endpoints.
- Graceful shutdown: stop accepting work → drain or reject outstanding messages → cancel timers
  → flush storage → terminate within a configurable deadline.
- Deploy/recovery docs: supported platforms + deps, config reference, upgrade/rollback, backup/DR.

## Phase 5 — RELEASE DISCIPLINE
- Continuously test Linux x86_64, Linux ARM64, macOS ARM64 (dev).
- Release gates: no sanitizer failures, no known P0/P1, all memory slopes bounded, 24h soak passes,
  native-Linux stress passes repeatedly, no unexplained compiler warnings.
- Semver + API/language compatibility rules + migration notes.
- **Commission an independent runtime/security review before calling it 1.0** — and fold in the
  deferred clean ETS re-audit (see Phase-1 verification gap).

---

## Invariants & landmines (don't trip these)

- **`struct sw_process` asm offsets are pinned**: `ctx`=0x70, `entry`/`arg` (arm64 0xF0/0xF8,
  x86_64 0xC0/0xC8), enforced by `_Static_assert(offsetof(...))` in `swarmrt_native.c`. **New
  fields go at the END of the struct** (after `gen_exec`/`on_destroy`) or you shift the offsets and
  the asm context switch corrupts. `swarmrt_asm.S` hardcodes these.
- **`on_destroy` hook**: per-process teardown callback fired in `process_destroy` on every exit
  (normal/kill/panic), snapshot-then-cleared so it can't double-fire; reset to NULL in
  `process_init_arena`. Frees GLOBAL-heap state only (never the arena). Arm it pre-runnable via
  `sw_spawn_dtor`/`sw_spawn_link_dtor`.
- **`_sw_free_global_val`** (studio.h) recursively frees a `deep_copy_global` value tree (maps +
  fun captures included) — use it for global-heap value graphs. **Do NOT** call it on a RAW
  (non-value) payload — it will misread it as `sw_val_t` and crash.
- **clangd lies on this C codebase** (it parses as C++): `void*`→`sw_val_t**` init errors,
  narrowing, goto-into-scope, writable-strings are all false positives. Trust real `cc` /
  `make swc libswarmrt` (zero warnings is the real gate).
- **BSD `sed` (macOS) has no `\b`** — use `[^A-Za-z0-9_]` boundaries.

## Conventions
- Branch → `git merge --ff-only` to `main` → push → delete branch. Targeted `git add`, never `-A`.
- End commit messages with: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- Public copy stays minimal/honest — never overstate (don't claim "done" or "zero leaks" without a
  gate that has been seen to FAIL without the fix).
