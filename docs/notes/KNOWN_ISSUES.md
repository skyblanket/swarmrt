# Known issues

Tracked publicly because users will hit them. Each open issue should have
a repro, impact, and current hypothesis.

## Open

These are genuine limitations, not crashes. Each is reproducible with the
shipped `bin/swc`.

### Compiled `receive` has no default timeout (interpreter/compiled divergence)

A bare `receive` with no `after` clause blocks forever in a compiled binary
(codegen emits an infinite wait), whereas the interpreter defaults to a 5s
timeout. The compiled behavior is the correct Erlang-style selective receive;
the divergence is the issue.

**Impact:** code that relies on the interpreter's implicit 5s timeout will hang
when compiled. **Workaround:** add an explicit `after MS -> ...` clause to any
`receive` that might not match, so compiled and interpreted runs behave the same.

### Interpreter recursion depth is bounded (no TCO in the tree-walker)

The interpreter (`swc run` / REPL / `swc test`) evaluates on the C stack with
a stack-margin guard and no tail-call optimisation; deep recursion raises a
clean, uncatchable `interpreter recursion depth exceeded` panic (exit 1). The
exact ceiling is environment-dependent (real stack headroom is measured);
assume a few hundred frames. Compiled binaries TCO self-tail-calls to
unbounded depth (gated by `tests/sw/test_tco_depth.sw`).

**Impact:** recursion-heavy programs must be compiled. **Workaround:**
`swc build` — the interpreter is for short scripts, tests, and the REPL.

### P1 (latent): spin-gated scheduler deadlock under depth-1 cross-scheduler ping-pong

With the idle-loop spin enabled (`SW_SPIN_US>0`), a depth-1 ping-pong
between two processes on different schedulers (~32KB payloads — the
gc-slope message probe shape) deadlocks in ~15% of runs: both fibers
parked WAITING, every runq empty, and ZERO enqueues from then on
(sched-trace telemetry: `enq=0`, `park_to≈6600/s`; the ~27% CPU of a
wedged process is pure park churn). 0/60 wedges with spin off, 9/60
with spin on. The publish-idle-then-repoll guard in the park path does
NOT close it, so the in-flight message/wake is lost upstream of the
park — suspects: the runq Vyukov push racing the spinning consumer's
pick, or the receive waiting-flag handoff. The spin therefore ships
**opt-in, default off**; the measured upside once fixed is 58.4 →
4.5µs per cross-scheduler round trip.

**Repro:** `tests/stress/spin_wedge_hunt.sh` (first wedge exits 1).
**Diagnosis aid:** `SW_SCHED_TRACE=1` prints 1Hz scheduler counters.
**Fix path:** TSan build + atomics audit (roadmap Phase 2.3/2.4); wire
the hunt script into the stress gate once the race is closed.

### Blocking C-call builtins occupy their scheduler OS thread

The curl-backed HTTP client builtins (`http_get` / `http_post` /
`http_request` / `http_post_stream`) and other synchronous C calls block
the scheduler THREAD, not just the calling fiber. Consequence: a program
that runs an in-process server fiber AND calls itself over HTTP
deadlocks under `SW_SCHEDULERS=1` — the blocked client holds the only
scheduler, the server fiber never runs. At `SW_SCHEDULERS=2` the same
deadlock fires whenever the client and server fibers happen to be placed
on the SAME scheduler: a runnable fiber in a blocked scheduler's local
queue is unstealable (work stealing covers only the global overflow
queue, not peer local queues). Found by the Phase-2.2 scheduler-count
matrix (test_http_request hung forever single-sched, all three loopback
tests failed at S=2; they now SKIP below 3 schedulers and the test
runner bounds every test with a 180s timeout). The deadlock watchdog did
not flag this shape — the thread is busy inside libcurl, not parked.

**Impact:** single-scheduler deployments must not self-call over
blocking clients; chatty blocking I/O also steals a core from every
other process on that scheduler. **Workaround:** `SW_SCHEDULERS>=2` for
self-loopback workloads (the WebSocket client is yield-aware and not
affected). **Fix direction (Phase 3):** run blocking transports on a
dedicated I/O thread pool with fiber park/wake, like `wsc_*` does.

### Mutual tail recursion is not TCO'd; deep chains overflow silently

Only **self** tail calls are optimised. Two functions tail-calling each other
(the natural state-machine shape) consume a C stack frame per hop and overflow
the 128KB fiber stack at depth ~10^4–10^5 — currently as a raw SIGSEGV without
even the crash banner (the handler runs on the overflowed fiber stack; needs
`sigaltstack`). See [REVIEW_FABLE_2026-06.md](REVIEW_FABLE_2026-06.md), O2.

**Impact:** mutually recursive FSMs die at depth in compiled binaries.
**Workaround:** keep the loop in one function and dispatch on an argument
(`fun fsm(state, n) { case state { ... } }`).

### No static type or shape checking

`sw` is dynamically typed by design — there is no compile-time type or arity
checking. A typo'd variable name compiles cleanly and becomes an atom at
runtime instead of erroring:

```sw
print(undefined_var)   # compiles; prints :undefined_var
```

**Impact:** name typos surface as silent runtime atoms rather than compile
errors. This is a deliberate tradeoff (matching the dynamic, Erlang-shaped
model), recorded here so the behavior is not a surprise.

## Recently cleared

### `try/catch` caught builtin panics in the interpreter but not compiled

Cleared on 2026-06-09 (Round-7 follow-up). The error model is now identical
on both paths and gated by the dual-path conformance runner
(`tests/sw/run_conform.sh`, wired into `make test-sw`):

- `error()` unwinds the full dynamic extent to the nearest `try` — an
  `error()` raised inside a callee lands in the caller's `catch`, and the
  statements after the raise do NOT run. (Compiled: per-process
  setjmp/longjmp chain, `sw_self_try_chain`; the old codegen ran the whole
  try body and tested the sentinel once at the end, so the statement after
  an `error()` still executed — and could panic the process before the
  catch was consulted.)
- `error()` outside any `try` stays the documented silent
  continue-with-nil on both paths (the interpreter previously unwound the
  entire rest of the program, silently).
- Panics (builtin panics, `panic()`, failed `expect()`) are uncatchable on
  both paths: `try/catch` does not absorb them, the run exits 1.
  `assert_raises` remains the sanctioned test-only interceptor.

### No `swc run` subcommand

Cleared. `swc run file.sw` exists and runs the tree-walking interpreter
(parse → merge imports → interpret `main()`; see `run_file` in `src/swc.c`).
The interpreter path (`swc run`, REPL, `swc test`) shares the runtime's
builtins with the compiled path.

### Multi-head cons patterns are unimplemented

Cleared on 2026-06-03. The list-pattern parser now accepts any number of
leading heads before the bar, so `[a, b | rest]` (and `[a, b, c | rest]`,
etc.) parse into a right-nested cons chain `cons(a, cons(b, rest))`. This
matches a list of length >= 2, binding `a`/`b` to the first two elements
and `rest` to the remainder — identically in the interpreter and the
compiler (verified by `tests/sw/test_patterns_codegen.sw`). Construction
position (`[a, b | rest]` building a list) works too.

### High-process-count spawn/send stress crash

Cleared on 2026-05-29 after re-testing the 80k-spawn send/receive stress
bench on native Linux x86_64:

- Host: `sushi`, Ubuntu 24.04, Linux 6.17, AMD EPYC 9554, 128 CPUs.
- Default multi-scheduler variant: 50/50 completed, 0/50 crashed.
- `SW_SCHEDULERS=1` variant: 50/50 completed, 0/50 crashed.

The stress gate now defaults to a strict threshold: every run in both
variants must print `ok 80000`. Lower thresholds can still be supplied
manually with `SW_STRESS_THRESHOLD` for exploratory bisects, but CI and
normal reviewer runs should treat any crash as a regression.
