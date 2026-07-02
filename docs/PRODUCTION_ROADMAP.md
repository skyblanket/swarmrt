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
| 2 | Runtime hardening (race detection, atomics, alloc-failure, soak) | ✅ **DONE** (2.1–2.6); full 24h run pending a host |
| 3 | Security & fault isolation (fuzzing, limits) | ⏳ fuzz boundaries ✅; limits/quotas ✅ (proc-mem quota, HTTP idle timeout, msg-size cap, backpressure decision); WS/SQLite fuzz + cross-proc-isolation-under-fuzz remain |
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

### 2.1 Linux LeakSanitizer CI leg  ✅ **DONE** (Round-7 continuation, 2026-06-09, on Linux)
- `make lsan-gate` (Linux-only): `tests/gc/lsan_lifecycle.sw` churns every lifecycle owner
  (timers fired + cancelled incl. pre-trampoline kill, static + dynamic supervisors killed,
  ETS replace/delete, spawns, compound messages; ~64 KB captures), exits cleanly, and LSan
  asserts zero definitely-lost blocks at exit. Suppressions in `tests/gc/lsan.supp`, each tied
  to a documented accepted-minor. **Proven bidirectional** (an injected unreachable block fails
  it). Advisory CI leg added to linux-quickstart.yml — promote to blocking once green a week.
- Phase-1 validation: 40 rounds of full lifecycle churn → **zero unsuppressed leaks**.
- Canary-writing lesson: at `-O1` clang elides an unused `malloc` — escape the pointer through
  a `volatile` global or your leak canary tests nothing.

### 2.2 Scheduler-count matrix  ✅ **DONE** (same session) — found a real architecture issue
- Suite + conformance run under `SW_SCHEDULERS=1/2/4/8`. **Finding:** the curl-backed HTTP
  client builtins block their scheduler OS THREAD, so self-loopback tests (in-process server +
  blocking client) **deadlock forever under SW_SCHEDULERS=1** — invisible at default counts,
  and the deadlock watchdog does not flag it (the thread is busy in libcurl, not parked).
  Documented in KNOWN_ISSUES; those tests SKIP at S=1; `run_tests.sh` now bounds every test
  with a 180s timeout so a hang FAILS instead of wedging the suite. Real fix is the Phase-3
  item below (blocking transports → I/O thread pool with fiber park/wake, like `wsc_*`).
- Cross-scheduler wake cost (Round-7 O4): bounded spin-before-park in the scheduler idle loop.
  Measured: cross-sched ping-pong **58.4 → 4.5 µs/rt (13×)**; spawn/exit −26%. **BUT the spin
  gates a latent scheduler race** — depth-1 ping-pong deadlocks ~15% of runs with spin on
  (0/60 off, 9/60 on; both fibers WAITING, zero enqueues — see KNOWN_ISSUES P1). Shipped
  **opt-in (`SW_SPIN_US`, default 0/off)** with reproducer `tests/stress/spin_wedge_hunt.sh`
  and `SW_SCHED_TRACE=1` telemetry. Root-cause in 2.3/2.4, then flip the default back on.

### 2.3 ThreadSanitizer build + race test  ✅ **DONE** (Round-7 continuation, on Linux)
- `make tsan-gate`: depth-1 message ping-pong (spin off AND on) + the full 80k spawn storm
  under `-fsanitize=thread`; fails on any unsuppressed race. **The feared fiber false-positive
  storm did not materialize**: cross-thread fiber migration synchronizes through the runq's C11
  atomics, so TSan sees the happens-before edges — no fiber annotations needed.
- Three real C11 races found and fixed: `sched->active` / `should_exit` / `g_swarm->running`
  were `volatile int` (not synchronization) — now `_Atomic int` (sw_scheduler is not
  asm-offset-pinned; cold flags, cost irrelevant); the sched-trace flag initialized after
  thread creation — now before.
- Suppressions (`tests/stress/tsan.supp`): ONLY the documented warn-only lock-free watchdog
  scanner (`watchdog_thread_fn`, `sw_io_active_port_count`). Atomicizing `sw_process.state`
  for a scanner-clean runtime is a 2.4 decision (it is written on every context switch).
- The spin-gated P1 deadlock does NOT reproduce under TSan (40/40 clean, spin on — timing
  perturbation suppresses the interleave) and shows no C11 race: it is a PROTOCOL bug
  (legal-but-wrong interleaving of the waiting/idle flag handshakes). Root-cause needs
  protocol reasoning / model checking against `tests/stress/spin_wedge_hunt.sh`, not TSan.

### 2.4 Make shared state atomic-or-locked  ✅ **DONE** (Round-7 continuation) — runtime TSan-clean
- **The P1 spin-gated deadlock is root-caused and fixed**: Dekker StoreLoad in the receive
  waiting-flag handshake (release-store `waiting` then acquire-load `sig_head` — no ordering
  across different objects). All `waiting` participants are now seq_cst; 0/300 wedge runs
  post-fix; spin default back ON (cross-sched ping-pong 58.4 → 3.0 µs/rt, 19×). The wedge
  AUTOPSY (`SW_SCHED_TRACE=1/2`: stall detector + state dump + event ring) stays in the
  runtime as the standing diagnosis tool. Gate: `tests/stress/spin_wedge_hunt.sh`.
- **FIXED this round** (TSan-driven, verified clean + perf-neutral):
  - `sw_process.state` → `_Atomic` with the 30 hot writes as explicit `atomic_store relaxed`
    (free codegen, reads stay implicit/free on x86; same layout — the asm-offset
    `_Static_assert`s pass). Closed the scheduler-swap-vs-`sw_monitor`/`sw_send_after`
    races. Perf unchanged: spawn 4.2 µs, ping-pong 2.6 µs.
  - Timer list: `fire_timers`' unlocked head-peek now reads under `tl->lock` (raced the
    locked insert; uncontended in the common no-timer case).
  - Monitor-at-death: `sw_monitor`'s already-dead path reads `exit_reason`/`panic_msg` and
    registers all under `link_lock`, and `process_exit` finalizes `exit_reason` under the
    same lock — closes the data race AND makes DOWN delivery exactly-once.
  - Test harness: `static volatile int` flags in test_phase2/4/5 → `_Atomic` (volatile is
    not synchronization), `__sync_*` → C11 `atomic_fetch_add`.
- **`make tsan-gate` covers msg, msg+spin, 80k storm, phase 2, phase 5 — all clean.**
- **ALSO FIXED** (the supervisor crash-restart races that surfaced once test timing shifted):
  - `exit_reason` → `_Atomic`: user/runtime code writes `sw_self()->exit_reason = N`
    directly (the b3_crasher idiom) while sw_monitor/process_exit read it.
  - `reg_entry` → `_Atomic`: a child's `process_exit` reads it while `sw_register` (from
    `sup_start_child` on another scheduler) writes it.
  - `kill_flag` → `_Atomic` (was volatile): scheduler-loop read vs cross-thread exit-signal
    write.
  - `registry_remove_proc` hashes the name UNDER the registry wrlock (was computed before
    the lock, reading entry->name that a concurrent reuse rewrote).
  - `process_destroy` frees `panic_msg` under `link_lock`, and sw_monitor's already-dead
    path COPIES it under the same lock — closes a use-after-free (a late monitor reading a
    panic_msg the teardown was freeing).
- **`make tsan-gate` covers msg, msg+spin, 80k storm, AND phase 2/4/5 — all clean.** The
  runtime is TSan-clean on the GenServer / Supervisor / Agent / DynSup / StateMachine /
  ProcessGroup paths plus message-passing and the spawn storm.
- Suppressed by design (tests/stress/tsan.supp): the warn-only watchdog scanner + `sw_stats`
  debug printer.
- (The former P1 deadlock was a protocol bug, not a data race — TSan stayed silent; found via
  the autopsy + seq_cst fix, see above.)

### 2.5 Allocation-failure safety  ✅ **DONE** (Round-7 continuation, Linux/ASAN)
- `make alloc-fault`: built with `-DSW_ALLOC_FAULT` + ASAN, sweeps `SW_FAIL_ALLOC_AT=1..120` so
  the Nth value/region allocation fails exactly once (atomic countdown in
  `sw_alloc_fault_tick`, wired into `val_alloc`'s global-heap fallback and every varena
  `chunk_new`). Each run must end clean OR with a loud OOM/spawn panic; the gate FAILS only on
  an ASAN memory error.
- Result: **120 fail-points, 0 memory errors** (100 graceful degradations — region alloc
  fails → spawn falls back to a global-heap arg copy / errors out cleanly; 20 loud-OOM aborts
  on the value path). The documented hazard (spawn region adopted but child died → leak or
  double-free) does not occur: the cleanup branches hold under injected failure.
- Zero production cost: all injection is behind `#ifdef SW_ALLOC_FAULT`, compiled only by the
  gate. Advisory-then-blocking CI leg added to linux-quickstart.yml.
### 2.6 Soak  ✅ **harness DONE; CI 60s smoke green** (full 24h pending a dedicated host)
- `make soak` (tests/soak/run_soak.sh + soak.sw): a mixed production-shaped workload — actor
  fan-out with ~8 KB message round-trips + supervisor crash/restart + ETS put/replace/delete
  + one-shot & interval timers + a long tail loop — runs for `SOAK_SECONDS` (default 60)
  while sampling RSS, and asserts clean exit (PROBE_OK, no watchdog/crash on stderr) + peak
  RSS under `SOAK_RSS_BUDGET_MB`.
- Local result: **20s → 2364 rounds, peak RSS 53 MB** (arena base ~48 MB, so ~5 MB working
  set over the entire mix — dead flat); 180s confirms the same. CI runs the 60s smoke
  (advisory/blocking leg in linux-quickstart.yml).
- **Remaining (needs infra, not code):** the full **24-hour** run is the same binary —
  `SOAK_SECONDS=86400 SOAK_RSS_BUDGET_MB=512 ./tests/soak/run_soak.sh` — on a dedicated
  Linux host. Assert bounded RSS + zero sanitizer hits + no leaked process slots over the
  full day. This is a sign-off that requires a sustained host run; the harness is ready.
---

## Phase 3 — SECURITY & FAULT ISOLATION  ⏳ (fuzz + depth limits DONE; quotas/backpressure remain)

**Done (Round-7 continuation):**
- Fuzz every external-input boundary under ASAN/UBSAN, 20k mutations each, wired into CI
  (`make fuzz`): parser, **JSON decoder** (the agent-facing boundary), distribution
  `sw_unmarshal`, HTTP header parser. fuzz-json found and fixed **two real heap-buffer-overflows**
  on its first run (number force-advance past NUL; backslash-as-last-byte in a string), plus a
  latent incomplete-free in `sw_val_free`'s map case.
- Recursion/decode-depth guards verified on every recursive decoder: parser
  (`SW_PARSE_MAX_DEPTH` + node budget + stack-headroom probe), JSON (`SW_JD_MAX_DEPTH` /
  compiled `SW_JSON_MAX_DEPTH`), distribution unmarshal (`SW_UNMARSH_MAX_DEPTH`).
- Parser can't OOM swc (the receive-clause spin fix + per-parse node budget, Phase-3 fuzz item).
- **Mailbox depth cap** (`SW_MAILBOX_MAX`, default 1M, `0` disables): every USER send
  (`sw_send`/`sw_send_tagged`/`sw_send_tagged_msg`, which covers local, dist-inbound and HTTP
  deliveries) is admission-checked against a per-process counter (`proc->mb_len`, at struct END —
  asm offsets). Over-cap messages are DROPPED, loudly (global `sw_mailbox_dropped()` counter +
  rate-limited stderr) and leak-free (VALUE region bulk-freed / RAW payload freed, mirroring
  process_destroy's unread-queue ownership), and the receiver is still woken (a skipped wake =
  livelock). **Exempt**: EXIT/DOWN signals (deliver_signal) and timer fires — supervision and
  `receive … after` survive a flood; a dropped gen_server CALL surfaces as the caller's existing
  receive timeout, never a hang. Cap is approximate under concurrency (overshoot ≤ #concurrent
  senders). Semantics deviation from BEAM (which penalizes senders instead of dropping) is
  deliberate: drop-with-counter is the bounded-memory choice for untrusted inbound. Gate:
  `make mailbox-flood` (bidirectional: capped run proves exact-cap admission + exemptions;
  `SW_MAILBOX_MAX=0` run proves zero false drops and full delivery).
- **HTTP request-size cap** (`SW_HTTP_MAX_REQUEST`, default 32MB, min 4096): bounds the
  per-connection rx buffer growth in `conn_on_data` (was unbounded `realloc` — one client could
  force ~4GB RSS) and rejects oversized declared `Content-Length` with a 413. WS frames were
  already capped at 16MB.

**Done (Phase-3 limits & quotas batch, 2026-07-03, branch `phase3-quotas`):**
- **Per-process memory quota** (`SW_PROC_MEM_MAX`, bytes; 0/unset = unlimited): enforced at the
  varena grow/adopt cold paths on `varena->total_bytes`. An over-quota PROCESS dies with a loud
  panic naming the quota + pid; the node and siblings survive (a supervisor can restart it).
  Scope: the per-process value arena — the `SW_GC_OFF`/global-heap fallback is not metered.
  Gate: `make quota-gate` (bidirectional).
- **HTTP idle timeout** (`SW_HTTP_IDLE_TIMEOUT_MS`, default 30000; 0 disables): connections with
  no inbound bytes are closed and their slot freed — slow-loris can no longer pin the
  `SW_HTTP_MAX_CONNS` table. Swept from the existing bridge fiber (periodic receive timeout, no
  new thread); each bridge sweeps only connections it owns. Established WS conns are exempt by
  default (`SW_HTTP_WS_IDLE_TIMEOUT_MS`, default 0 = never — quiet LiveView/agent sessions are
  legitimate). Gate: `make slowloris-gate` (bidirectional; phase 2 reproduces the DoS).
- **Max local message size** (`SW_MSG_MAX_BYTES`, bytes; 0/unset = unlimited — deliberate
  default, dist frames are already bounded by `SW_NODE_MAX_FRAME`): enforced in
  `sw_send_tagged_msg` on the message region's `total_bytes`. Over-cap sends are dropped
  loudly (rate-limited stderr + `sw_msgsize_dropped()` counter), leak-free (region bulk-freed),
  and the receiver is still woken (livelock guard). EXIT/DOWN exempt by construction
  (`deliver_signal`). Gate: `make msgsize-gate` (bidirectional).

**Decision record — sender-side backpressure: NOT building (2026-07-03).** The mailbox cap
keeps drop-with-counter semantics rather than throttling senders. Rationale: (a) bounded memory
against *untrusted/hostile* inbound is the security property Phase 3 needs — a sender-throttling
scheme gives a flooder a lever to slow the node instead; (b) cooperating processes that need
lossless flow already have it: gen_server CALL (request/reply) self-throttles, and
`Std.task_stream` bounds fan-out; (c) BEAM's sender-penalty model exists to protect a shared
global heap — swarmrt messages are copied into per-message regions, so the pressure point the
BEAM design protects does not exist here. Revisit only on a real user report, as an opt-in
(`SW_MAILBOX_BLOCK=1`-style) never a default. **Per-node memory quota: deferred** — the
per-process quota × process count bounds value memory operationally, and node-level ceilings
are the OS/deployment layer's job (cgroup/ulimit/container limits, documented in Phase 4's
deploy docs) rather than a second in-runtime accounting pass.

**Remaining (feature work):**
- Extend fuzz to the WebSocket frame parser + the `db_*`/SQLite arg path.
- Prove malformed input cannot crash another process (cross-process isolation under fuzz).

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
