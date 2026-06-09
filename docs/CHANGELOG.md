# SwarmRT changelog

Recent commits, newest first. Strict format: date, headline, what changed, what unblocked.

---

## 2026-06-09 — bounded memory: supervisor child-start closures (master + per-incarnation copy)

**fix(gc): supervisor child-start closures are reclaimed — without racing live children.** A
supervisor deep-copies each child's start closure (`spec.start_arg`) to the global heap so it
can re-apply it across restarts; before, that closure was never freed (leaked on every
permanent child removal and at supervisor teardown — unbounded under start_child/terminate
churn or crash-restart storms). The naive fix — free it at the removal/teardown site — is a
**use-after-free** (confirmed by ASAN): `sw_process_kill` is asynchronous and the closure *is*
the child's running code (the child lives inside `sw_val_apply(c->fn)` for its whole life), so
freeing it while the child is still unwinding through it faults; and restarts re-use the same
closure, so the child can't free it either.

Fix: a **master + per-incarnation-copy** ownership split. The supervisor keeps the closure as
the MASTER (`spec.start_arg`) and frees it (`sw_spec_free_start_arg`) only on permanent child
removal / supervisor teardown — at which point no live child references it. Each (re)start
hands the child a FRESH copy (`spec.copy_start_arg` → `_sup_copy_child_closure`), which the
child frees in its own `process_destroy` via a new per-process `on_destroy` hook
(`swarmrt_native.h`, fired on EVERY exit path incl. panic — a panic `sw_context_swap`s to the
scheduler and never returns through the entry fn, so a fiber-tail free would be skipped). The
master and the copies are distinct allocations, so the async kill is harmless and there's no
double-free. Native-C child specs (app children) set neither fn pointer and are untouched.

**test(gc): slope + four ASAN scenarios.** `tests/gc/slope_dynsup_churn.sw` → `make gc-slope`
(budget 8 MB): start_child/terminate_child churn holds flat (1 MB growth 2k→20k; pre-fix
neutered = 28 MB → FAILs). `tests/gc/sup_restart_repro.sw` + `slope_dynsup_churn.sw` → `make
gc-stress` (ASAN + `-DSW_ARENA_POISON`): dynamic churn UAF check + a crash+restart+teardown
double-free tripwire (permanent child crashes repeatedly → restarted with a fresh copy each
time, master reused, copies freed per dead incarnation on the panic path). Also ASAN-verified:
static `one_for_all` teardown with multiple children. gc-stress (7 gates), gc-slope (9 probes),
phases 2-10 (incl. 14/14 supervisor + 12/12), test-sw 53/475 all green; zero warnings.

**Phase 1 of the production roadmap is complete:** all generated execution state per-process
(`_sw_error` + line/file/trace), and every long-lived ownership path bounded — ETS values,
timer closures (delay + interval), and supervisor child closures. (Cron needed nothing —
`lib/Cron.sw` is pure-sw on `receive ... after N`, already arena-freed.) The separate C-API
`sw_ets_*` in `swarmrt_ets.c` (interpreter/phase-test path, short-lived) is the one known
residual, deferred.

## 2026-06-09 — timers: interval/delay are now cancellable + non-blocking (and interval frees its closure)

**fix(timer): `interval`/`delay` no longer block a scheduler thread or run uninterruptibly.**
The timer entries `_every_entry`/`_after_entry` slept with raw libc `usleep` inside a plain C
loop — which blocks the whole scheduler OS thread for the sleep (starving every co-located
fiber) and, worse, never yields, so a killed timer was **uninterruptible**: `kill_flag` was
never observed, `process_destroy` never ran, the interval spun forever, and its global-heap
closure leaked without bound. Both now wait via the yielding, kill-aware `sw_receive_any`
(the same primitive `_builtin_sleep` uses): it swaps back to the scheduler each tick (no
starvation) and returns on kill, after which the loop unwinds and frees its own closure.
So a cancelled `interval` actually stops, the scheduler reaps it, and `_sw_free_global_val`
reclaims the deep-copied closure (captures included). `delay` likewise becomes cancellable
before firing (and still frees its one-shot closure). Cron is unaffected — `lib/Cron.sw` is
pure-sw on `receive ... after N`, already yielding/cancellable, arena-freed.

**test(gc): `tests/gc/slope_interval.sw` → `make gc-slope` (budget 6 MB).** Each round starts
an interval whose closure captures ~1 KB, lets it park, `exit_proc`-cancels it, and waits for
it to be reaped — bounded concurrency, so RSS must be flat (0 MB growth 2k→12k rounds). The
probe doubles as a **cancellability regression test**: pre-fix the interval can't be killed,
so it hangs (no `PROBE_OK`) and the gate fails on timeout. Verified ASAN+poison-clean (no
double-free of the closure). gc-stress (both modes, 5 gates), gc-slope (8 probes), phases
2-10, test-sw 53/475 all green; zero warnings.

**Still global-heap (next): supervisor child-spec closures.** Freeing them on child
removal/teardown is NOT a simple leak fix — `sw_process_kill` is async and the closure IS the
child's running code (the child lives inside `sw_val_apply(c->fn)`), so freeing it at removal
races the live child (confirmed heap-UAF). Needs a master + per-incarnation-copy ownership
split (supervisor frees the master with no live referencer; each child frees its own copy on
exit). Deferred to a careful pass.

## 2026-06-09 — bounded memory: one-shot timer (delay) closures are freed after firing

**fix(gc): repeated `delay(ms, fn)` no longer leaks a closure per call.** `delay` deep-copies
`fn` to the global heap (it fires after the caller may have exited) and spawns a one-shot
timer process; that process applied the closure once then freed only its small struct, never
the deep-copied closure graph — so every fired `delay` leaked. `_after_entry` now frees the
whole closure (captures included) via `_sw_free_global_val` after its single apply returns;
nothing else holds that private copy (the apply result is discarded), so it's safe. (The ETS
recursive-free helper `_vets_free_val` is generalised + renamed `_sw_free_global_val`, now
shared by ETS and timers.)

**test(gc): `tests/gc/slope_timer.sw` → `make gc-slope` (budget 8 MB).** A bounded-concurrency
probe (fire one delay, wait for its reply, repeat — so process/stack memory stays flat and the
closure leak is the only thing that could grow) holds **flat** RSS (0 MB growth 2k→20k rounds;
pre-fix grew 28 MB → FAILs). gc-stress (both modes, 5 gates), gc-slope (7 probes), test-sw
53/475 all green; zero warnings.

**Still global-heap (next):** `interval`/cron closures leak only on cancel (the forever-looping
timer process holds the closure; freeing it needs a per-process destructor hook) and supervisor
child-spec closures leak on supervisor teardown — both are leak-on-teardown of dynamically
created entities (bounded live cost), deferred to a focused pass.

## 2026-06-09 — bounded memory: ETS values are freed on replace/delete (BEAM copy-out semantics)

**fix(gc): a high-churn ETS table no longer grows without bound.** The compiled-backend
ETS store (`_vets_*` in `swarmrt_builtins_studio.h`) copied values IN on insert (global
heap, so they outlive the inserting process) but never freed them: `ets_put`/`update`/
`cas`/`update_counter` replace paths overwrote the stored pointer without freeing the old
value, and `ets_delete`/`ets_take` freed only the entry struct, not the value (and never
the key). A long-lived table hammered with replace/delete leaked a value graph per
mutation. Now the table OWNS its stored key+value and frees them on every replace, delete,
and take (`_vets_free_val` — a full recursive free that, unlike `sw_val_free`, also reclaims
MAP elements and FUN captures, safe because ETS values are independent `deep_copy_global`
trees with no interned/shared nodes).

**The enabling change — `ets_get`/`ets_take` now COPY the value OUT** into the reading
process's arena (`sw_val_deep_copy_local`), done under the table lock. This is BEAM's ETS
semantics (a lookup copies the term into the calling process) and is what makes
free-on-replace safe: a reader never holds the table's pointer, so freeing the stored copy
can't dangle it. `ets_update`'s lambda runs outside the lock, so the old value is snapshotted
as a copy first (else a concurrent free during the call would be a UAF).

**test(gc): two permanent gates.** `tests/gc/slope_ets.sw` (wired into `make gc-slope`,
budget 10 MB): a fixed 32-key live set hammered with put-replace/update_counter/delete/take
holds **flat** RSS (0 MB growth 2k→20k rounds; pre-fix grew 19 MB → FAILs the 10 MB budget).
`tests/gc/ets_alias_repro.sw` (wired into `make gc-stress`, `SW_SCHEDULERS=1` + ASAN +
`-DSW_ARENA_POISON`): a holder parks holding a looked-up value while a churner replaces+
deletes that key 60×, then deep-reads it — clean post-fix; pre-fix (sharing the raw pointer)
it's a confirmed `heap-use-after-free in _builtin_to_string`. gc-stress (both modes, 5 gates),
gc-slope (6 probes), phases 2-10 (arena fully reclaimed), test-sw 53/475 all green; zero
warnings. Remaining global-heap owners: supervisor child closures + timer/cron closures
(next). (The separate C-API `sw_ets_*` in `swarmrt_ets.c` — interpreter/phase-test path,
short-lived — is not yet converted.)

## 2026-06-09 — process isolation: panic traces (line/file/call-stack) are per-process too

**fix(gc): a panic now reports the panicking process's OWN location and call chain.**
The generated line/file trackers (`_sw_current_line`, `_sw_current_file`) and the
call-stack ring buffer (`_sw_trace`/`_sw_trace_top`/`_sw_trace_overflowed`) were
scheduler-thread thread-locals. The call DEPTH is shared across every fiber on the OS
thread, so a process parked in a blocking op at some depth, plus another fiber running
(and parking) at a different depth, left the shared trace state mixed — a third process
that panicked printed an **unrelated process's call stack**. (Memory-safe — these hold
only `.rodata` function-name literals + ints, never a freeable value, so this was a wrong
*diagnostic*, not a UAF — but the roadmap rightly wants correct traces.) Fix: this state
now lives in a per-process `sw_gen_exec_t` (`swarmrt_native.h`) reached through `_sw_gen`,
which the scheduler points at the running process on every context switch; the generated
`_sw_current_line`/`_sw_current_file`/`_sw_trace*` macros (`swarmrt_builtins_studio.h`)
redirect through it. Each process's trace starts at depth 0 and only ever holds its own
frames. The block is lazily allocated and kept with the slab slot (like `stack_mem`) —
reset on reuse, freed at slab teardown — so peak memory still tracks concurrently-live
processes, not slab capacity (phase2 still reclaims 100000/100000; gc-slope flat).

**test(gc): permanent stderr gate.** `tests/gc/trace_xproc_repro.sw` +
`scripts/gc_trace_check.sh` (wired into `make gc-stress`, `SW_SCHEDULERS=1`): a deep,
distinctively-named contaminant parks ~forever while a short-chained victim panics; the
harness asserts the victim's panic call chain contains only its own frames (`xvictim_*`)
and **no foreign frames** (`xcontam_*`). Proven bidirectional — pre-fix (shared TLS) the
victim's banner lists the contaminant's `xcontam_e..a` frames; post-fix it's `xvictim`-only.
gc-stress (both scheduler modes, 4 gates), gc-slope, phases 2-10, test-sw 53/475 all green;
zero warnings. With this, ALL generated execution state (`_sw_error` + line/file/trace) is
per-process — generated-code process isolation is complete; remaining global-heap owners
are ETS values + supervisor/timer closures (next).

## 2026-06-09 — process isolation: generated `_sw_error` is per-process, not scheduler-thread-local

**fix(gc): a process can no longer catch another process's error (was a heap-UAF).** The
generated `error(x)` / `try`/`catch` carrier `_sw_error` was a `__thread` (scheduler-thread)
variable. `sleep()`/`receive()` inside a `try` compile to a fiber-yielding C call, so a
process parked in `try { sleep(...); ... }` would resume to find `_sw_error` set by an
**unrelated** process that ran on the same scheduler thread meanwhile — wrongly entering its
`catch`. Worse, that foreign error value lived in the *other* process's arena and was freed
when it exited → **heap-use-after-free** when the wrong catch read it (ASAN: UAF in
`_builtin_to_string`, freed via `sw_varena_free_all`←`process_destroy`). Fix: `_sw_error`
is now `(*sw_self_error_slot())` — a per-process `gen_error` slot at the **end** of
`sw_process_t` (shifts no asm-pinned offset), with a `__thread` fallback only for
non-process (REPL/C) contexts. The error value lives in the catching process's *own* arena,
reclaimed with it. Codegen no longer emits the `__thread _sw_error`.

**test(gc): permanent bidirectional gate.** `tests/gc/error_xproc_repro.sw` (now wired into
`make gc-stress`, run under `SW_SCHEDULERS=1` + ASAN + `-DSW_ARENA_POISON`) parks a victim in
`try { sleep(300); 'MARKER' }` while 12 short-lived contaminants spew `error(...)` and exit.
It **FAILS** on the pre-fix thread-local (wrong cross-process catch → exit 1, *and* an ASAN
heap-UAF) and **PASSES** on the fix (`PROBE_OK`, victim returns its own body). gc-stress
(both scheduler modes), gc-slope, phases 2-10, test-sw 53/475 all green; library build is
now literally **zero warnings** (`g_old_sigaction` marked unused on macOS).

**Audited the remaining generated thread-locals — diagnostic-only, not UAF.**
`_sw_current_line`/`_sw_current_file`/`_sw_trace[]`/`_sw_trace_top`/`_sw_trace_overflowed`
hold only compile-time literals (`.rodata` strings + ints), written by codegen and read only
by the panic/stderr path — nothing freeable. Cross-fiber contamination there yields at most a
**wrong file/line/stack in a panic banner**, never a memory-safety bug. (Moving these to
per-process for *correct* traces is the next roadmap item, not a blocker.) Two runtime TLS
slots — `g_pending_spawn_region` and the `sw_self_error_slot` fallback — are safe by the
"schedulers run only fibers" invariant but flagged for documenting/guarding in hardening.

## 2026-06-08 — Ownership v2: lifecycle-owned escaped values + scoped turn-checkpoint

**feat(gc): escaped memory is now reclaimed, not just safe.** GC v1 deep-copied escaped
values (messages, spawn args) to the global heap where they leaked; a fixed-concurrency
workload passing large values grew with cumulative jobs, and long-lived loops accumulated
every turn. Ownership v2 gives every escaped graph exactly one lifecycle owner:
- **Spawn regions** (`swarmrt_codegen.c`): a spawn's args/captures (incl. the `spawn(f)`
  lambda form that used to leak) are copied into a right-sized region the child **adopts**
  into its arena on entry (O(1) chunk splice) and frees at exit; spawn failure reclaims it.
- **Message regions** (`swarmrt_native.c` + codegen): `sw_send_value` copies the payload
  into a per-message region (discriminated from RAW signal/struct payloads by a `pkind`
  tag); a compiled `receive` match **adopts** it; `process_destroy` bulk-frees undelivered
  ones; C-level `sw_receive*` materialize a free-able global copy (keeping their contract).
- **Scoped turn-checkpoint** (`swarmrt_codegen.c` tail-call): a tail-recursive function
  records an arena "floor" at entry and, past a threshold above it, copies the recursion
  args into a temp region, `sw_varena_reset_to(floor)` to reclaim *this function's* per-turn
  garbage (never the caller's values below the floor — a naive whole-arena reset corrupted
  callers, caught by gc-stress), and splices the args back.

**test(gc): the slope gate goes from RED to GREEN.** `make gc-slope` (now a CI gate, run
under default + `SW_SCHEDULERS=1`) proves bounded memory: a fixed-width 32 KB-arg spawn
workload (200→2000 jobs), a fixed-depth large-message stream (500→4000), and a 100k-turn
loop all hold **flat** peak RSS (each was +87 / +169 / unbounded MB before). gc-stress
ASAN+poison stays green (the floor-scoped reset and adopt-splice are UAF-clean), test-sw
53/475, phases 2-10, `make stress`, check-docs, doctest all green. SW_GC_OFF preserved.

**Remaining (honest), still global-heap (reclaimed at OS exit / table destroy):** ETS
values (a high-churn ETS replace/delete loop grows until table destroy), supervisor child
closures, and timer/cron closures; the interpreter has no arena (short-lived).

## 2026-06-08 — GC v1: per-process value arena, freed on exit + copy-on-escape

**feat(gc): memory is reclaimed.** Until now every `sw_val_t` lived until the OS process exited — a long-running or high-spawn agent leaked without bound, which made swarmrt unusable for anything real. GC v1 (`swarmrt_varena.{c,h}`) gives each runtime process a **value arena**: every value it builds while running lives there and is freed wholesale in `process_destroy` — *after* the fiber returns, so there are no live C-stack roots to enumerate (why an arena beats refcount/tracing for v1). The value constructors route through `val_alloc`/`val_strdup`, keyed on `sw_self_varena()` (arena when a fiber runs; global heap for the interpreter / on escape). Measured: 50k spawn-and-exit workers hold **~493 MB with the arena vs ~4,491 MB and climbing without it** (`SW_GC_OFF=1`) — peak RSS now tracks concurrently-live processes, not cumulative spawns.

**feat(gc): copy-on-escape re-establishes BEAM's no-shared-heap invariant.** Compiled `send` shared the raw `sw_val_t*` cross-process, so a naive per-process free would be a use-after-free. `sw_val_deep_copy_global` now rebuilds the value DAG on the global heap, funnelled through a type-safe `sw_send_value()` choke point (its `sw_val_t*` param makes passing a runtime struct a compile error). Every value escape is routed: send dispatch + remote delivery, spawn struct/lambda captures + pmap, ETS stores, pubsub (per-subscriber), http handler delivery, supervisor child closures, timer closures. Cross-process messages are deep-copied (O(message size)); the ~10ns figure is now just the enqueue.

**test(gc): two independent adversarial workflow waves, 24 surfaces, zero defects.** `make gc-stress` compiles a copy-on-escape stress harness under ASAN + `-DSW_ARENA_POISON` (freed chunks filled with `0xDE`) — a missed deep-copy surfaces as a use-after-free or a `0xDE`-garbage content assert. Validated across registered-name sends, monitor/link exit reasons, gen_server call/cast, Cron timers, nested spawn, binaries, closures-as-messages, wide/DAG/edge values, ETS churn, multi-message workers, concurrency ping-pong (6400 crossings), supervised crash+restart, depth-cap boundary, undelivered-message teardown, and a mixed soak. `scripts/gc_asan_run.sh <test.sw>` runs any test under the same gate. test-sw 53/475, fuzz clean.

**Honest scope:** v1 does NOT reclaim mid-life within a single long-lived process (bump-allocation can't free inside a living fiber) — prefer a worker-per-job shape; loop-reset is the next GC step. Values nested deeper than 256 levels truncate on cross-process copy. `SW_GC_OFF=1` reverts to the pre-GC global-heap behaviour (escape hatch). README/ARCHITECTURE/BENCHMARKS/BUILDING_AGENTS updated to match (incl. LoC: ~12K runtime + ~8.5K studio + ~4K compiler + ~5.5K interpreter; ~50K total `src/`).

---

## 2026-06-03 — headless polish + doctest harness (the "docs lie" tripwire)

**fix(swc): silence resolver "cannot open './X.sw'" noise on successful imports.** The build-path import resolver probes four candidate paths per import (CamelCase + lowercase × input-dir + stdlib) and used the noisy `read_file` for each probe, so every *successful* stdlib import (`import Std`) printed two spurious `swc: cannot open '...'` lines to stderr from the candidates it tried before the hit. Switched the candidate probe to the silent `read_file_quiet` (the `swc run` path already used it). A genuine total-resolution failure still prints the clear `swc: cannot resolve import 'X' (looked in .../ and .../)` error. Headless/piped agents no longer eat phantom error lines on a clean build.

**feat(runtime): `SW_RUNTIME_QUIET` suppresses the startup banner.** A built binary printed two `[SwarmRT] Arena initialized…` banner lines on every run. They already honored `SW_QUIET=1`, but that flag is also a compile-time knob; `SW_RUNTIME_QUIET=1` is the runtime-only switch a headless agent sets in the *binary's* environment to keep captured streams clean. Banner stays on stderr; either flag silences it.

**fix(runtime): deadlock watchdog no longer false-positives on idle I/O servers.** An idle TCP/HTTP server parks every live process in `receive` with an empty mailbox (the main loop and the `http_listen` bridge both wait for the I/O thread to deliver an accept/data event), which the watchdog read as `stuck_count == live_count` and warned "possible deadlock". It now also calls the new `sw_io_active_port_count()` and stays silent while any live port could still wake a process. Regression test `tests/sw/watchdog/idle_http_server.sw` (wired into `make test-sw`) asserts an idle server runs clean; verified it fires 5 spurious warnings without the guard.

**fix(docs): SQL-injection example replaced with parameterized form.** `docs/BUILDING_AGENTS.md` taught a string-interpolated `db_exec(db, f"INSERT ... '{escape(user_input)}'")` — which both modeled SQL injection AND called a non-existent `escape()` builtin. Replaced with the parameterized `db_exec(db, "INSERT ... VALUES (?, ?)", ["user", user_input])` 3-arg form (verified to neutralize a `'); DROP TABLE` payload).

**feat(ci): doctest harness — `scripts/doctest.sh` + `make doctest`.** `check_sw_docs.sh` proves doc snippets COMPILE; doctest proves they RUN and produce the OUTPUT they claim. Every complete `\`\`\`sw` block carrying `# => EXPECTED` markers is compiled, run with `SW_QUIET=1`, and its stdout asserted line-for-line. Drift fails non-zero. Wired into `make doctest` and the Linux CI workflow alongside `check-docs`. Three doctests now run (18 output lines asserted): two hello-world blocks + a `BuiltinDemo` block pinning the previously-undocumented builtins.

**docs(SW_LANGUAGE): document builtins the dogfood audit named.** Added `abs`, `to_float`, `ord`, `typeof`, `is_list`, `is_map`, `map_new`, `map_get(m,k,default)` (3-arg), `bytes_from_ints`, `file_read_bytes`/`file_write_bytes`, `db_exec(h,sql,[args])` (3-arg binds), `wsc_connect_tls`/`wsc_set_handler`, and a `Math` (`import Math`) reference section. Fixed the `format()` example: it used f-string `#{}` syntax (`format("hi {} (#{})", ...)` produces `hi world ()`) — corrected to positional `{}` placeholders, with a note that inline-expression interpolation is an f-string feature. Confirmed `ets_list` is documented as `{k, v}` tuples (matches shipping shape) and `swc run` is documented (exists).

---

## 2026-06-03 — http_post_stream: tagged return + SSE "data:" no-space fix

**feat(builtins): `http_post_stream` returns a tagged result.** The streaming chat builtin used to return a bare OpenAI-shaped JSON string, so an empty `{"choices":[{"message":{"content":""}}]}` conflated three distinct outcomes an agent must tell apart: the model genuinely said nothing, the SSE stream failed to parse, and curl couldn't connect. It now returns a 2-tuple the caller `case`-branches on — `{'ok, json}` on success (json is the same OpenAI-shaped string, so existing `json_decode` → `choices[0].message.content` extraction keeps working after unwrapping) and `{'error, reason}` on failure. Failures are classified: curl/transport (`"curl exit 7: Connection refused"`, captured from curl stderr), non-2xx HTTP (`"HTTP 404: {server error body}"`, detected via `--write-out '%{stderr}...'` so the status code is read back without polluting the SSE stdout), and empty-or-unparseable stream (`"stream produced no content and no tool calls..."`). Interrupts (ESC/Ctrl-C) and `finish_reason="length"` truncation stay `{'ok, ...}` — the partial content they carry is real, not a failure. Streaming-to-stdout, the reasoning channel, tool-call reassembly, usage scraping, and subagent message routing are all unchanged.

**fix(builtins): SSE parser dropped spec-legal `data:{...}` (no space).** The stream line parser hard-required the 6-byte prefix `"data: "` (with trailing space). The WHATWG SSE spec makes that space *optional* — `data:{...}` is legal and emitted by some servers. Those chunks were silently `continue`d: the read loop ran but the `delta.content` extractor never fired, so the call returned empty content indistinguishable from "model said nothing". Root-caused against a localhost SSE stub. The parser now matches the 5-byte `"data:"` prefix and skips at most one optional leading space, per spec. A non-2xx error body that arrives as a final newline-less line is also now flushed into the `{'error, ...}` reason (was dropped because the per-line handler only fires on `\n`).

**test:** new `tests/sw/test_http_post_stream.sw` (wired into `make test-sw`) stands up a swarmrt `http_listen` server as a fake OpenAI endpoint and drives `http_post_stream` against it end-to-end with zero live credentials — asserts `data: ` (space) and `data:` (no-space) both yield `{'ok}` with correctly-accumulated content, a 404 yields `{'error, "HTTP 404: ..."}`, and a dead port yields `{'error, "curl exit ..."}`.

**Updated callers/docs:** `examples/llm_agent.sw` (case-branches the tagged result), `docs/BUILDING_AGENTS.md` (tagged-result pattern + exact SSE shape the parser expects), `docs/SW_LANGUAGE.md` (builtin table). Unblocks the dogfood chat agent, which previously could not get non-empty content from a valid localhost SSE stub and could not distinguish an empty turn from a transport failure for retry logic.

---

## 2026-06-02 — docs: correct DOWN tuple to shipping 5-tuple

**docs(SW_LANGUAGE): `monitor` DOWN message is a 5-tuple.** SW_LANGUAGE.md documented the monitor down message as a 4-tuple `{'DOWN', ref, pid, reason}`, but the codegen has always emitted the Erlang-shaped 5-tuple `{'DOWN', ref, 'process', pid, reason}` (`emit_receive` in `swarmrt_codegen.c`). Corrected the doc to match the shipping shape rather than changing codegen (lower risk). Down-message handlers must match five elements with `'process'` in the third slot. Unblocks model-written `monitor`/DOWN code that was matching the wrong arity.

---

## 2026-05-31 — deadlock watchdog + exec_argv + assert_raises + module globals + docs fix

Five features landed together after the post-release hardening pass.

**feat(runtime): deadlock watchdog.** A background thread wakes every `SW_DEADLOCK_MS` milliseconds (default 5000) and checks whether every live non-scheduler process is in `SW_PROC_WAITING` state with an empty mailbox. If so, it prints a one-line warning to stderr (`[swarmrt] WARNING: all N processes blocked in receive — possible deadlock`) and flushes. The check is best-effort (no locks held) so a message in-flight at the exact scan moment can produce a false positive. The watchdog is warn-only — it does not terminate processes. Disable with `SW_DEADLOCK_DETECT=0`; tune the interval with `SW_DEADLOCK_MS=<ms>`.

**feat(builtins): `exec_argv(cmd, args)`.** Fork+exec with no shell — takes a command string and a list of argument strings, builds `argv[]` directly, and returns `{exit_code, stdout_string}`. No shell metacharacter interpretation, no injection surface. Prefer over `shell()` whenever inputs come from user data or tool arguments. Available in both compiled code and the REPL. Documented in SW_LANGUAGE.md and API_REFERENCE.md.

**feat(builtins): `assert_raises(fn, msg)`.** Assert that a zero-arg lambda panics (or calls `error()`) with a message containing `msg`. The `swc test` runner intercepts the panic before it reaches `exit(1)`, records the assertion result, and continues running the suite. Use in `tests/sw/test_*.sw` files alongside `assert_eq` / `assert_ne`. Documented in SW_LANGUAGE.md section 11 and API_REFERENCE.md section 16.

**feat(language): module-level `let` globals.** Modules may now declare up to 16 named constants at the top level using `let`. Initialized once at compile time, read-only thereafter, visible to every function in the module. Supported literal types: `int`, `float`, `string`, `atom`. Complex expressions and collection literals are not supported at module scope — put those in `fun init()`. Any assignment to a module-global name inside a function creates a local shadow; the global itself is unchanged. Documented in SW_LANGUAGE.md section 2.

**docs: remove false `sys_exit(0)` requirement.** SW_LANGUAGE.md section 1 previously implied `sys_exit(0)` was needed at the end of `main()`. Corrected: the runtime exits cleanly when `main()` returns (Go-style cond-var signal → `sw_shutdown(0)`). `sys_exit(code)` is only needed when exiting with a non-zero status to signal an error to the calling shell.

---

## 2026-05-29 — interactive-IO correctness + back-pressure primitive

Three fixes shaken out of long-running interactive sessions in swarm-code.

**test(stress):** re-tested the 80k-spawn send/receive stress workload
on `sushi` (Ubuntu 24.04, Linux 6.17, native x86_64, AMD EPYC 9554):
50/50 default-scheduler runs and 50/50 `SW_SCHEDULERS=1` runs completed
with zero crashes. The previous spawn-storm race is no longer tracked as
open. `make stress` now defaults to a strict threshold: all runs in both
variants must complete. `SW_STRESS_THRESHOLD` remains available for
exploratory bisects, but CI/reviewer runs should treat any crash as a
regression.

**fix(codegen):** `receive` clauses now release the matched message
*before* evaluating the clause body, not after. The deserialized
pattern bindings are independent values (`deserialize_val` copies out
of the message), so freeing the mailbox slot early is safe and returns
mailbox pressure to the allocator one body-evaluation sooner. Applies
to both guarded and unguarded clauses. All 110 compiled + 16
interpreter assertions stay green, processes suite included.

**fix(io):** `print()` and `shell()`'s live-progress tail now honour
the `read_line` line editor. Previously any `print()` fired from a
receive handler, background tick, or wake chain — and `shell()`'s
`\r\033[K` progress wipe — wrote straight at the cursor and shoved the
user's in-flight typing into nonsense. All three sites now take
`_sw_term_lock`, emit, and redraw the editor's prompt+buf+cursor below
(the same dance `print_above()` already did). Headless / piped / no-TTY
paths collapse to the original behaviour — zero overhead when no editor
is up. Unblocked: usable interactive REPL-style agents that print
asynchronously while the user is typing.

**feat(builtins):** `pid_alive(pid)` — `kill(pid, 0)` wrapper returning
`'true'`/`'false'`. Lets schedulers and supervisors back-pressure
(skip dispatch while the previous child is still running) without
paying a `shell("kill -0 …")` poll per check. Accepts int or string;
`EPERM` counts as alive (process exists, we just can't signal it).
Surfaced by a runaway 5s cron in swarm-code firing new children every
tick while prior ones were stuck mid-LLM-call.

---

## 2026-05-25 — harness primitives + boot-speed parity

Four landings driven by the swarm-code v0.2 hardening pass.

**perf(sw_init):** new `SW_MAX_PROCS` env knob plus spin-wait
replacement for `usleep(10000)` after scheduler thread-start. Cuts
swc-binary boot from 36 ms → 25 ms (default), 21 ms with
`SW_MAX_PROCS=1024`. swarm-code is now at boot parity with goose
(Rust). Arena ceiling stays at `SWARM_MAX_PROCESSES` (100 k) for
backward compat; CLI tools that never spawn more than a handful of
sw processes should set `SW_MAX_PROCS` low.

**feat(builtins):** atomic filesystem primitives.
- `file_rename(src, dst)` — wraps `rename(2)`
- `file_stat(path)` — returns `%{size, mtime, mode, is_dir, exists}` or `nil`
- `file_atomic_write(path, content)` — writes `path.tmp.<pid>` then
  `rename(2)` to `path` (crash-safe for session journals / cron state)
- `file_temp(prefix)` — wraps `mkstemp` with `<prefix>XXXXXX`

**feat(ets):** atomic mutation ops on the per-process ETS.
- `ets_update_counter(t, k, delta, initial)` — atomic `+=`; seeds
  `initial + delta` if missing. Returns new int as a *fresh* value
  (no aliasing into earlier callers' bindings).
- `ets_cas(t, k, expected, new)` — compare-and-swap, `'true'`/`'false'`
- `ets_take(t, k)` — atomic get-and-delete
- `ets_update(t, k, fun)` — placeholder; needs a runtime helper to
  invoke a `.sw` lambda from a C builtin (cas/get-put loops in the
  meantime)

**test(make):** added `test-core` (alias for the old test-all
contents) and `test-full` (core + OTP + phase2..10 + search +
mcp + sws + examples). `test-all` kept as a backward-compat alias
for `test-core`. CI should gate on `test-full`.

Three of the four were drafted by parallel headless swarm-code -p
subagents — the first time SwarmRT shipped runtime-level fixes
proposed by an agent built on top of SwarmRT. ETS counter needed
two manual bug-fixes after a smoke test (pointer aliasing into
the entry's value, and missing delta on first-insert).

---

## Current state — what's in the build

As of `1e3769a` (2026-05-31), the `.sw` language has:

- **Core:** `module / fun / export / import`, `spawn / send / receive`, `case`, `if / else`, `try / catch`, pattern matching with guards.
- **Values:** int, float, string, atom (`'ok'`), tuple (`{...}`), list (`[...]`), map (`%{key: val}`), pid, nil, fun. `map_get` treats atom and string keys interchangeably; `++` works on lists too.
- **Concurrency:** lock-free MPSC mailboxes, selective receive, ETS for shared mutable state. Supervisors (one-for-one / one-for-all / rest-for-one) plus `link`, `unlink`, `monitor`, `demonitor`, `exit_proc`, `trap_exit` — full Erlang fault-tolerance surface from userland sw.
- **Built-in I/O:** HTTP (server + client + streaming), WebSocket client/server, Chrome DevTools (browser automation), files, JSON, base64, shell (+ `shell_sandboxed` for sandbox-exec / firejail isolation), bidirectional subprocesses (`subprocess_*`), SQLite (`db_open / db_exec / db_query`).
- **Ergonomics:** f-strings (`f"hi {name}"`), `format("hi {} count {}", n)`, `++` polymorphic, variadic `print`, `;` works in any block, C-reserved words legal as identifiers, `case` for top-level pattern dispatch. Module-level `let` globals (up to 16 literal constants, immutable).
- **New builtins (2026-05):** `exec_argv(cmd, args)` → `{code, out}` (fork+exec, no shell injection), `pid_alive(pid)` → `'true'`/`'false'`, `print_above(msg)` (non-clobbering print during read_line), `assert_raises(fn, msg)` (test-suite panic assertion), `file_rename/file_stat/file_atomic_write/file_temp` (atomic filesystem primitives), `ets_update_counter/ets_cas/ets_take` (atomic ETS mutation).
- **Stdlib (lib/, auto-imported from `<swarmrt>/lib/`):**
  - `Std` — list/map/string helpers (range, take, drop, zip, partition, group_by, sort, unique, contains, find, any, all, count, last, init, chunk_every, intersperse, max_by, min_by, sum, product, string_join, string_pad_*, string_repeat, string_indent…)
  - `Prompt` — `{{var}}` template engine (file or string source)
  - `Cron` — `every(ms, fn)` / `at("HH:MM", fn)` / `in_ms(ms, fn)` wake scheduler
  - `Telemetry` — event hub with stdout / file / JSONL sinks
  - `Mcp` — Model Context Protocol client + server (JSON-RPC over stdio)
  - `Embed` — OpenAI-compatible embeddings client
  - `Vec` — ETS-backed cosine-similarity vector store
- **Tooling:** `swc build / emit / repl / test / lsp`, `--target=<triple>` cross-compile (`zig cc` or matching cross-gcc), per-statement `#line` directives, did-you-mean for unknown function names, tree-sitter grammar at `tree-sitter-sw/` for editor highlighting.
- **Error story:** `panic(msg)` / `expect(value, msg)` for unrecoverable cases, `error(msg)` + `try/catch` for recoverable ones. `hd`/`tl`/`elem`/divzero panic loudly. Panics now print the full **call chain** with `module.fn at src/X.sw:N` per frame.
- **Runtime:** programs exit when `main()` returns (Go-style) — `sys_exit(0)` at end of `main()` is not required. `sys_exit(code)` is only needed for non-zero exit status. 100K+ concurrent processes per node, ~150ns context switch. `SW_MAX_PROCS` env trims the arena ceiling for CLI tools. Deadlock watchdog enabled by default (`SW_DEADLOCK_DETECT=0` to disable, `SW_DEADLOCK_MS` to tune).

Sw test suite: **8 compiled `.sw` files (114 assertions) + 1 interpreter file (31 assertions) + 9 C-side phase test files (75 C assertions)**. swarm-code is the canonical real-world consumer; rebuilds clean against every commit.

The `eval/` directory holds an LLM code-gen benchmark: 10 prompts × 3 models (Kimi K2.6 / K2.5 / Moonshot v1) measured pass rate on single-shot generation. See `eval/results/results.md`.

---

## 2026-05-21 — Reviewer-driven hardening (rounds 2–7)

Six rounds of external review (Claude web agent + Codex). Each round
filed a markdown report against the latest commit; this entry
consolidates what each round shipped. The full narrative is at
[`docs/notes/REVIEW_HARDENING.md`](notes/REVIEW_HARDENING.md), the
remaining open item is the lone entry in
[`docs/notes/KNOWN_ISSUES.md`](notes/KNOWN_ISSUES.md).

**Round 2** — first deep audit. Fixed: per-process panic recovery
(replaced `exit(1)` with scheduler `longjmp` so a panic in one process
no longer kills the runtime), HTTP POST body delivery (length-prefix
off-by-one), ETS enumeration (`ets_list`/`ets_count` always returned
empty), scheduler count auto-detect (was hardcoded 2),
compile-time arity check + did-you-mean suggestions + halt-on-unknown
function, hot-reload doc honesty. Distribution now uses a
type-preserving binary marshal/unmarshal instead of JSON so tuples
stay tuples and atoms stay atoms over the wire.

**Round 3** — process lifecycle fixes. `spawn(fun() {...})` lambdas
returned pid 0 silently (only N_CALL spawns registered a trampoline);
now goes through a generic lambda trampoline. EXIT/DOWN signal reasons
arrive as the panic message string instead of an opaque `-1`. First
attempt at the arena slot-reuse race documented since round 2:
1-slot deferred-free per scheduler.

**Round 4** — distribution + heavier race attempt. Added
`SW_VAL_REMOTE_PID` so `self()` over the wire becomes a routable pid
on the receiver; `sw_send_dispatch` routes by type; receiving node
auto-registers the sender on first packet so `send(from, reply)`
works in the natural read. `arena->next_pid` starts at 1 (pid 0 is
the no-pid sentinel — the very first spawned process used to be
silently invisible to `sw_find_by_pid`). 1-slot ring widened to
64-slot deferred-free.

**Round 5** — distribution framing + deterministic race fix. (1) TCP
framing: `dist_handler` used to consume one length-prefixed frame
per `PORT_DATA` event and drop the rest; per-peer `rx_buf` + drain
loop fixes both coalesced reads and split reads (verified: 10k
back-to-back messages, 50KB payloads). (2) Arena race: ripped out
the 64-slot ring (it actually regressed 40k/50k thresholds —
allocation-rate pressure, not the right shape). Replaced with the
deterministic ABA pattern: per-slot `_Atomic generation` +
`sw_spinlock_t ctx_lock`. `sw_safe_swap_into` copies ctx to a
stack-local under the lock, re-checks the generation, then calls a
new asm `sw_context_swap_from_copy` that reads from the local
copy — `process_init_arena` can no longer tear the swap. New asm
added for x86_64 SysV, x86_64 Windows, and ARM64.

**Round 6** — message envelope leak + audit cleanups. `emit_receive`
used to leak the `sw_msg_t` envelope on every matched receive — the
per-thread `tls_msg_free` freelist stayed empty and `msg_alloc`
always missed straight to `malloc`. Exposed `sw_msg_release()` and
wired it into the codegen so envelopes return to the freelist; the
payload `sw_val_t` is left alive on purpose since pattern bindings
alias subparts of it. CI stress initially widened to 50 runs at a 90%
threshold across both multi- and single-scheduler variants (the
round-5 fix closed the original race deterministically, while a
different spawn-storm crash still needed follow-up at the time). The
May 29 sushi retest later cleared that race and the stress gate is now
strict: every configured run must complete. Round-4 audit
cleanups: `pmap` accepts either arg order like
`map`/`filter`; `map_has_key` matches `map_get`'s atom-vs-string
fallback; `expect(nil, msg)` now panics (the literal `nil` lexes to
atom `'nil'`, not `SW_VAL_NIL`, so the previous strict-type check
fell through and silently returned the atom).

**Round 7** — Codex review caught CI was green while seven phase
tests were failing locally (`make test-sw` runs only the .sw
language suite, not the C-side runtime tests). One root cause for
all seven: `sw_spawn_link` set `tls_scheduler` to a non-parent
scheduler to avoid cooperative-deadlock, but `sw_spawn_opts` picks
its scheduler from the global `next_sched` round-robin counter and
**ignored `tls_scheduler` entirely** — the whole "force child to a
different scheduler" block was a no-op. Children would routinely
land on the parent's scheduler, get stuck behind the parent's
`usleep`/blocking-receive, and the test would see 2/3 group members
instead of 3/3. Fix: separate TLS slot `tls_spawn_override`,
honoured by `sw_spawn_opts` when non-NULL. After the fix all 9 phase
test files pass 100% — phase 2 (GenServer/Supervisor), 3 (ETS, 15
tests), 4 (Agent/App/DynSup, 14 tests), 5 (StateMachine/PG, 12
tests), 6 (TCP, 6), 7 (hot reload, 5), 8 (GC, 5), 9 (distribution,
4), 10 (search + fsindex). All wired into CI. README example now mirrors
`examples/counter.sw` verbatim so what you see is what `./counter`
prints (`Count: 8` + `Counter stopped at 8`).

May 29 follow-up: the spawn-storm race no longer reproduces on native
Linux x86_64 (`sushi`: 50/50 multi-scheduler and 50/50
single-scheduler completed). It has been removed from open known issues
and the stress gate is strict by default.

---

## 2026-05-20 — REPL builtin parity + eval/ benchmark

The previous self-critique flagged two embarrassments: (a) the REPL
knew ~30 builtins while compiled code knew ~100, so first-time users
would hit "undefined function" in the REPL for things like
`file_read` or `db_open` that worked fine in `.sw` files; (b) the
"for AI agents" pitch had no empirical backing.

Both fixed.

**REPL builtin parity (`src/swarmrt_lang.c`):** added an
`interp_extra_builtin()` dispatch helper that handles the pure-functional
surface compiled code already had: `file_read/write/append/exists/delete/list/mkdir`,
`db_open/exec/query/close` (SQLite), `shell`, `shell_sandboxed`,
`string_replace/sub/truncate`, `panic / expect / error`, `sleep / random_int /
getenv`, `map_merge / map_remove`, `json_get / json_escape`. Process-scheduler
primitives (`spawn / link / monitor / send / receive / trap_exit` and friends)
print a one-shot hint and return `nil` instead of silently dropping to
"undefined function" — the REPL doesn't simulate the full scheduler, so
those still require `swc build`.

Two new tests guard against drift recurring:
- `tests/sw/test_repl_builtins.sw` — runs through the compiled path
  (27 assertions), exercises the new builtins.
- `tests/sw/repl/test_repl_builtins_interp.sw` — runs through `swc
  test` (interpreter, 16 assertions), uses the 2-arg builtin
  `assert_eq` directly to avoid the user-defined-vs-builtin
  assert_eq shadowing.

`run_tests.sh` now scans both paths.

**`eval/` benchmark:** structured directory with 10 tasks
(`hello_world`, `fizzbuzz`, `list_sum`, `json_parse`, `actor_counter`,
`case_dispatch`, `sqlite_crud`, `pipe_filter`, `fault_tolerance`,
`http_pipeline`), a system prompt distilled from `docs/SW_LANGUAGE.md`,
and a `runner.sh` that POSTs to each model in `models.json`, extracts
the generated `.sw` from a ```sw fence, compiles via `swc build`,
runs the binary, and diffs stdout against the prompt's expected
output. Results land in `eval/results/<run_id>/summary.md` with the
latest mirrored to `eval/results/results.md`.

First baseline run pinned in `eval/results/results.md`. Findings
documented there — the eval surfaces several real language quirks
(nested case-as-RHS doesn't parse, f-strings need the `f` prefix,
multi-line receive arm bodies need explicit semicolons) which are
now tracked as follow-up bugs and folded into the next system_prompt
revision.

---

## 2026-05-18 — The agent-building story, told properly

We'd shipped every primitive but never written down "use sw to BUILD AI
agents" coherently. README mentioned HTTP + WebSocket + Chrome as
builtins but didn't explain why those specifically + process model =
agents. AGENT_SYSTEM.md is for LLMs *writing* sw on demand; that's a
different audience from a developer *building* an agent with sw.

This fills the gap.

**New: `docs/BUILDING_AGENTS.md`.** The developer-facing guide. Lays
out the model (process = agent, mailbox = inbox, recursion = state),
the agent-loop skeleton, calling an LLM (sync + streaming + subagent-
mode multiplexing), tool-call parse + `case` dispatch, the studio
pattern, ETS for shared state, supervisor strategies, Chrome via CDP,
MCP via WebSocket, and a "when the runtime feels wrong" debug list.

**New: `examples/llm_agent.sw`.** A real LLM-driven agent in ~90
lines. Takes a question, calls an OpenAI-compatible endpoint via
`http_post_stream`, parses `<tool name="…">{…}</tool>` tags out of
the response, dispatches with `case`, appends results to history,
loops until the model emits a final answer with no tools. Set
`API_KEY` and run.

**README.** New "Building AI agents" section with the killer
primitive table, pointing at the new doc + example. Documentation
table now distinguishes the two agent docs: BUILDING_AGENTS.md (devs
building agents) vs AGENT_SYSTEM.md (LLMs writing sw).

---

## 2026-05-18 — Loud failure, panic/expect, did-you-mean

Self-critique pass said runtime errors were the biggest gap — silent
nils everywhere, no stack-trace context, no compile-time hints when a
function name was a typo. This commit closes those.

**Runtime line / file tracking.** Codegen now emits
`_sw_current_line = N; _sw_current_file = "src/Mod.sw";`
alongside each `#line` directive so the runtime knows where the
program is at any moment. Two thread-locals in the preamble, one extra
store per source line — negligible cost.

**panic(msg) + expect(value, msg) builtins.**
- `panic(msg)` prints a red `panic: <msg>\n  at src/X.sw:N` to stderr
  and `exit(1)`. Cannot be caught. Use for impossible states /
  invariant violations.
- `expect(value, msg)` is the idiomatic unwrap — passes value through
  when non-nil, panics with msg when nil. Replaces the explicit
  `if (x == nil) { panic("...") }` boilerplate.
- Distinct from `error(msg)` (which is catchable by `try/catch`).

**hd / tl / elem / divide-by-zero now panic instead of returning nil.**
- `hd([])` → `panic: hd: list is empty at src/X.sw:N`
- `tl([])` → ditto
- `elem(tuple, 5)` when tuple has 3 elems → `elem: index 5 out of range for 3-tuple`
- `n / 0`, `n % 0` → `division by zero`
- New C helper `_sw_runtime_panic(fmt, ...)` does the printf-style
  panic; uses the runtime line/file trackers above.
- `map_get` and `ets_get` stay lenient — optional lookup with nil
  fallback is a real use case, not a bug.

**Compile-time "did you mean?"** for unknown function names. When
`swc` sees a call to a name that isn't a builtin, module function, or
declared variable, it prints suggestions via Levenshtein distance over
the builtin list plus the calling module's own functions:
```
src/Hello.sw:4: unknown function 'strng_length' — did you mean 'string_length'?
```
Threshold = min(3, len/2). Up to 3 suggestions, sorted by distance.
Hint is printed BEFORE the C compile runs so the user sees the
actionable fix first.

**Tests.** New `tests/sw/test_errors.sw` (5 assertions) covers expect
pass-through, try/catch caught + uncaught, and the scope-shadowing
regression from earlier today. Full suite is 48 assertions across 6
files. swarm-code rebuilds clean against the new compiler.

---

## 2026-05-15 — f-strings + showcase examples

**f-string interpolation.** The third headline LLM-ergonomics win:

```sw
f"hi {name} count {n}"                  # → "hi world count 42"
f"req={req_id} status={code} ms={elapsed}"
f"upper: {string_upper(name)}"          # any expression in {…}
```

Implementation is delightfully small: turns out the language already
had `"hello #{name}"` interpolation built in (`parse_interp_string`).
The lexer for `f"..."` rewrites top-level `{` to `#{` while scanning,
emits TOK_STRING, and the existing #{...} handler does the rest.
Inside an embedded expression we track brace-depth + inner-string
state so `f"name={get_name(\"key\")}"` parses correctly.

f-strings work in both compiled code and the REPL.

**Three new showcase examples** that flex the post-`case` / `format` /
f-string ergonomics:

- **examples/dispatcher.sw** — the studio-pattern actor in 50 lines.
  Tagged-message dispatch via `case`, state via the recursion arg,
  per-agent prefix lines via f-strings. The skeleton swarm-code's
  `agents.sw` started from.
- **examples/json_pipeline.sw** — JSON load → `case` classify by
  age band (with guards) → f-string render. Shows how the new
  ergonomics turn a deeply-nested if/else chain into something
  readable in 35 lines.
- **examples/http_echo.sw** — a working HTTP server in 25 lines.
  `case` on the path, f-strings for templating, `http_listen` +
  `http_respond` builtins. Hit with `curl http://localhost:8080/hello/sky`.

**5 new f-string tests** added to test_case_and_format.sw — full
suite is now 43 assertions across 5 files.

---

## 2026-05-15 — `case` expression, `format()`, REPL polish

The "make LLMs love it" pass. Three big language UX wins plus a few
sharp-edge fixes.

**`case` expression — top-level pattern matching.** The single biggest
ergonomic win for anyone (human or LLM) writing sw. Previously, dispatch
on a value meant nested `if/else` ladders — `is_recursive_tool` in
swarm-code was 4 levels deep, `char_ord` was 38 levels. Now:
```
case msg {
    {'ok', v}      -> "ok: " ++ to_string(v)
    {'error', why} -> "err: " ++ to_string(why)
    n when n > 0   -> "positive"
    _              -> "default"
}
```
Same arm-clause shape as `receive` (pattern, optional `when guard`,
body). Falls through to the next arm if a guard rejects.
Implementation: new `N_CASE` AST node, parser at `par_primary` (after
`try`), `emit_case` codegen mirrors `emit_if`'s scope-snapshotting
pattern wrapped in `do { ... } while(0)` so `break;` exits on first
match. Also added to the tree-walking interpreter so it works in the
REPL.

**`format(template, args...)` builtin.** Reduces the
`++ to_string(x) ++` noise that polluted every prose-with-data:
```
print(format("[{}] req={} ms={}", level, req_id, elapsed_ms))
```
`{}` placeholders consume the next positional arg; `{{`/`}}` escape
literal braces; missing args render as `{}` so you see the gap instead
of crashing. Composite values render via the new `sw_val_format` —
same shape as `print()` produces. Available in both compiled and REPL
paths.

**REPL is now actually useful.** `swc repl` already existed but the
tree-walker was missing `format`, `case`, `string_split`,
`string_contains`, `string_starts_with`, `string_ends_with`,
`string_index_of`, `string_upper`, `string_lower`, `string_trim`,
`string_length`, `json_encode`, `json_decode`, `map_size`,
`map_has_key`, `timestamp`, and tuple/list/map rendering in
`to_string`. Added all of those. Also: `length()` now works on maps
(was silently returning 0). Variables persist across lines, multi-line
input continues until brackets balance.

**Bugs fixed along the way.**
- `try/catch` had the same scope-shadowing bug we fixed in `if/else`
  earlier today — second `try/catch` with the same `err_var` name
  failed with "use of undeclared identifier". Snapshot/restore
  `ndeclared` around the catch block.
- `to_string` of tuples / lists / maps / pids was returning `<val:6>`
  garbage. Refactored: split `sw_val_print` into `sw_val_format(FILE *)`
  + a stdout wrapper, route `to_string` through a memstream-backed
  `sw_val_format` for composite values. Now you get `{ok, 42}`,
  `[1, 2, 3]`, `%{a: 1}`, `<pid:7>` — matching what `print()` shows.

**Tests.** New `tests/sw/test_case_and_format.sw` adds 17 assertions
covering case dispatch (literal / guard pass / guard fall-through /
tuple bind / atom match / catchall), format (basic / multi /
composite / escape / missing-arg), and the new map builtins. Total
suite is now 38 assertions across 5 files, runs `make test-sw` in
under 2s.

---

## 2026-05-15 — Generated programs exit when `main()` returns + repo polish

**The big one.** Until now, every sw binary's generated `main()` ended in
`while(1) usleep(...)`, so even a one-shot script like `examples/counter.sw`
would print its output and then hang forever. Users had to either
`sys_exit(0)` explicitly or Ctrl-C the process. Removed.

The new entry+main pair waits on a `pthread_cond` that `_main_entry` signals
when the user's `main()` function returns, then calls `sw_shutdown(0)` to
join the scheduler threads cleanly and `return 0` from the C `main`. Result:
every example in `examples/` now exits cleanly with code 0 after doing its
work. Long-running servers still work — they just have to put a permanent
`receive { ... }` or `sleep` loop at the end of `main` (Go-style, not
Erlang-style).

**Repo polish for the agent + human audiences.**
- README rewritten as a proper landing page — strong hero, clear positioning
  ("BEAM-shaped runtime for the AI-agent era"), agent-friendly highlights,
  comparison table.
- `docs/AGENT_SYSTEM.md` replaced. Was a stale architecture-design doc with
  "Status: 🔄 Needs..." energy. Now it's a one-page practical "Writing sw —
  for AI Agents" cheatsheet plus a recommended system-prompt snippet.
- `examples/multi_main.sw` patched to `import MathLib` explicitly (was
  missing the import — relied on a deprecated auto-resolution path).

---

## 2026-05-15 — sw test framework + browser screenshot inline + `;` in receive arms

**Test framework.** `tests/sw/test_*.sw` files + `tests/sw/run_tests.sh`
driver + `make test-sw` target. Each test file runs its assertions via
in-line `assert_eq` / `assert_true` helpers, prints `PASS <name>` /
`FAIL <name>: msg` lines, and `sys_exit(0|1)` based on rollup. The
driver compiles + runs each, totals across files. Initial coverage:
- `test_lang_basics.sw` — modulo, map literal, `;` in if-branch,
  scope shadowing, C-reserved-word identifiers (5 tests)
- `test_strings.sw` — `string_index_of` (hit / miss / empty),
  `string_split`, concat, base64 roundtrip + known vector (7 tests)
- `test_json.sw` — string / int / map roundtrip, `\uXXXX` decode,
  list roundtrip (5 tests)
- `test_processes.sw` — spawn + send + receive, self-pid, ets put/get,
  ets-missing-returns-nil (4 tests)

Total: 21 assertions across 4 files. `make test-sw` runs in <2s.

**`;` separator in receive arms.** Same fix as the if-branch one —
`receive { ... }` arm bodies now consume `TOK_SEMI` between
statements. The two paths share no parser code (receive arms have
their own loop, not `par_block`) so this needed a dedicated edit.

**Browser screenshot inline.** Now that `base64_decode` is a builtin,
`Browser.screenshot` decodes in-process and writes the binary PNG
directly via `file_write`. Removes the tmp file, the `base64 -d`
shell pipe, and the `rm -f` cleanup. ~10 LOC delta in
`swarm-code/src/browser.sw`.

---

## 2026-05-15 — Codegen polish: per-statement #line + C-keyword mangling + `;` in if-branches

Three small but high-leverage codegen / parser fixes that close out the
language papercuts list.

**Per-statement `#line` directives.** The function-level `#line` work from
earlier today (every function entry emits `#line N "src/Module.sw"`) is
extended: `emit_expr`'s N_BLOCK case now emits a `#line` before each
statement when its source line differs from the last emitted one. C
compiler errors now point at the *exact* failing sw line, not the
function's start. Throttled to one directive per source line so a
multi-expression line doesn't get spammed. Probe:
```
fun main() {
    a = 1
    b = 2
    c = bogus_undeclared_function(a, b)   ← error now points here
    print(c)
}
```

**C-reserved-word mangling.** sw identifiers matching a C keyword
(`inline`, `static`, `extern`, `const`, `register`, `volatile`, `auto`,
`goto`, `restrict`, `signed`, `unsigned`, `union`, `enum`, `struct`,
`typedef`, `return`, `break`, `continue`, etc.) used to error at the C
stage with confusing messages. Now `mangle_for_c(name)` appends `_sw`
at every C-emission site (assignments, function params, lambda captures,
pattern bindings, identifier reads). The AST + `ctx->declared` list
keep the original sw name, so `is_declared` lookups still work by
source-level spelling. Mangling is deterministic, so reads and writes
of the same variable always produce the same C-side name. 8-slot
rotating buffer keeps multiple `mangle_for_c(...)` calls in one
`fprintf` from clobbering each other.

**`;` as statement separator inside any block.** `par_block` now
consumes any leading `TOK_SEMI` tokens between statements, so
`if (x) { stmt1 ; stmt2 } else { stmt3 ; stmt4 }` parses and runs.
Pure superset — newlines (which the lexer was already eating as
whitespace) still separate statements as before.

---

## 2026-05-15 — Subagent stream multiplexing (studio polish)

The studio model promised "all subagent output flows through messages so
parallel agents don't interleave on the shared TTY." Until today, subagent
LLM streams went directly to stdout via the C `http_post_stream` builtin,
so `parallel([a, b, c])` produced unreadable interleaved output.

**Runtime change** — `http_post_stream(url, headers, body)` now accepts two
optional 4th + 5th args: `target_pid` and `name`. When supplied, the call
runs in **subagent mode**:
- No spinner, no ESC interrupt path, no inline TTY UI
- Each delta.content chunk is sent as `{'stream_chunk', name, text}` to
  `target_pid` (flushed at newlines for nice line-break boundaries)
- Each delta.reasoning_content chunk → `{'stream_reason', name, text}`
- A final `{'stream_done', name}` marks end of stream
- Truncation / curl-error markers also routed as message chunks instead
  of stdout writes
- Returns the same OpenAI-shaped JSON string so `extract_content` works
  unchanged in the caller

**swarm-code wiring**:
- `LLM.chat_for_subagent(messages, opts, target_pid, name)` calls the
  new builtin variant
- `Agents.run_agent_turn` dispatches to `chat_for_subagent` when a
  `main_agent` is registered (always true in normal use)
- `UI.stream_chunk_render / stream_reason_render / stream_done_render`
  use a dedicated ETS table (`stream_state_table`, single-key
  `'current'`) so chunks from the same agent merge inline and prefix
  lines only print on agent transitions
- Receive arms added to `agent.sw` main_loop, `agents.sw`
  `wait_for_reply_with` and `parallel_collect_with` so streams are
  drained whether main is idle, blocking on an `ask`, or collecting
  parallel replies

Default (3-arg) calls are unchanged — the TTY path is byte-for-byte
identical, so `swarm-code`'s own model output renders the same as before.

---

## 2026-05-15 — Language ergonomics pass

The "ten papercuts" pass on the sw language and codegen. None individually big, collectively the difference between fluent and frustrating.

**Operators**
- `%` is now a binary modulo operator. Was reserved for map-literal prefix only; users had to write `n - (n / d) * d`. Disambiguated by introducing `TOK_MAP_OPEN` for the `%{` form so `expr %{...}` keeps working.

**Builtins**
- `string_index_of(haystack, needle)` → int (-1 if not found). Userland was rolling its own `find_sub` helper every time.
- `base64_encode(s)` and `base64_decode(s)` exposed as builtins. Stops sw code shelling out to `base64 -d` for screenshots and similar.

**HTTP / JSON buffer growth**
- `http_post` response: was 512KB hard cap → now grows from 64KB. Long LLM replies no longer silently truncate.
- `http_get` response: same treatment.
- `http_get` content read (via `web_fetch`-style use): was 64KB hard cap → grows on demand.
- (Earlier 2026-05-11 fix: `json_encode` 256KB cap → grows. The "unexpected EOF" mystery is gone.)

**Codegen — error reporting**
- Emits `#line N "src/<Module>.sw"` at each function entry. C compiler errors now point back at the user's sw file (e.g. `src/main.sw:42: error`) instead of `/tmp/swc_Main_*.c:16000`. Editors can jump to the line. Off by a few lines from the exact failing statement — function-level, not statement-level — but a massive UX win regardless.

**Codegen — variable scope shadowing**
- The same sw variable name in two `if/else` branches no longer compiles to "use of undeclared identifier 'X'" at the C step. `emit_if` now snapshots `ndeclared` on entering each branch and restores it on exit, so sibling branches are independent scopes — matching the user's mental model.

**Repo hygiene**
- `.gitignore` covers the test-binary dirs that `make test-phase*` leaves at root (atelier, counter_test, error_test*, ets_test, hello_test, hello_test_bin, import_main, integration_test, my_test, patent_lab, research_lab, video_studio, …). `git status` is clean again.

---

## 2026-05-13 — Native CDP support

- New WebSocket *client* builtins: `wsc_connect / wsc_send / wsc_recv / wsc_close`. Existing `ws_*` was server-side only.
- New `chrome_launch(port?, headless?)` builtin. Discovers a Chromium binary across macOS / Linux paths (incl. Playwright's bundled chromium cache as fallback), spawns with `--remote-debugging-port` + isolated `--user-data-dir`, returns the port.
- Together these enable swarm-code to drive a real browser via CDP without Node/Playwright/Python dependencies.

---

## 2026-05-12 — Soft interrupt for http_post

- Refactored `http_post` from blocking `system()` to popen+select with stdin watching.
- ESC (0x1b) or Ctrl-C (0x03) during a model call now SIGTERM's the curl process group via `_sw_pkill_close`, returns sentinel `"__INTERRUPTED__"`.
- Caller (e.g. swarm-code's `chat_native`) detects the sentinel and returns nil cleanly — no spurious retry, no JSON-decode noise.

---

## 2026-05-11 — JSON parser fixes

- `json_decode` now properly decodes `\uXXXX` escapes to UTF-8 (handles surrogate pairs for SMP characters too). Previously dropped the `\` and emitted literal `u0026` etc., which broke shell composition in tool calls (`&&` arrived as `u0026u0026`).
- `json_encode` buffer auto-grows from 64KB instead of capping at 256KB. The "unexpected EOF" mystery in long agent histories is fixed.
- Both parser copies (`swarmrt_builtins_studio.h` for `json_decode` builtin, `swarmrt_lang.c` for `sw_lang_json_decode` used by node distribution) updated identically.

---

For detailed commit history: `git log --oneline` in the repo. For the language reference these features integrate into, see [SW_LANGUAGE.md](SW_LANGUAGE.md).
