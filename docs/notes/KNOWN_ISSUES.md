# Known issues

Tracked publicly because users will hit them. Each open issue should have
a repro, impact, and current hypothesis.

## Open

These are genuine limitations, not crashes. Each is reproducible with the
shipped `bin/swc`.

### Interpreter (`swc run`) first-spawn scheduling flake on constrained hosts

On the tree-walking `swc run` path (single cooperative scheduler), the FIRST
`spawn(closure_value)` in a fresh runtime occasionally does not schedule its
child, so the child's effect never appears even after ~1s of polling.
Subsequent spawns in the same run are unaffected — it looks like a
cold-scheduler race on the very first fiber.

**Impact:** interpreter/dev path only — the COMPILED path (the shipping
product) is unaffected and runs the child every time. **Repro:** only observed
on the 2-core GitHub-hosted Linux CI runner; NOT reproducible locally
(`test_spawn_value` interp passes 100/100 on multi-core macOS, even under load).
**Hypothesis:** a first-spawn/runqueue initialization race that only surfaces
under heavy core contention. **Workaround (in `tests/sw/run/test_spawn_value.sw`):**
re-spawn the idempotent closure if its effect hasn't appeared. A real fix needs
a Linux repro host to bisect the interp scheduler's first-fiber enqueue.

### Compiled `receive` has no default timeout (interpreter/compiled divergence)

A bare `receive` with no `after` clause blocks forever in a compiled binary
(codegen emits an infinite wait), whereas the interpreter defaults to a 5s
timeout. The compiled behavior is the correct Erlang-style selective receive;
the divergence is the issue.

**Impact:** code that relies on the interpreter's implicit 5s timeout will hang
when compiled. **Workaround:** add an explicit `after MS -> ...` clause to any
`receive` that might not match, so compiled and interpreted runs behave the same.

### Interpreter non-tail recursion is bounded

The interpreter (`swc run` / REPL / `swc test`) runs tail calls in place (see
"Recently cleared"), but a NON-tail call still nests C frames: plain recursion
like `fun sum_to(n) { n + sum_to(n - 1) }` raises a clean
`interpreter recursion depth exceeded` panic after a few hundred frames.
**Workaround:** write the loop tail-recursively with an accumulator, or `swc build`.

### A few blocking builtins still occupy their scheduler OS thread

`http_get`, `http_request`, `http_post` and `exec_argv` now run on the runtime's
offload pool (see "Recently cleared"). Still inline on the scheduler thread:
`http_post_stream` (streams chunks to the TTY / a parent process), `http_post` while
the interactive line editor owns the terminal (its ESC watcher reads the TTY),
`shell()`'s initial `system()` launch, `db_*` (SQLite) and `llm_*`. A long call there
blocks every other process queued on the same scheduler.

**Workaround:** `SW_SCHEDULERS>=2`, or move the call into its own process.

### The HTTP server never frees a connection's port struct

Closing a connection closes its socket (the per-connection fd leak is fixed), but the
~64-byte `sw_port_t` is not freed: the IO thread may still hold it in an event batch
fetched before the close, so freeing it safely needs an event refcount. About 64 MB
per million connections over a process lifetime.

### Compiled mutual tail recursion is not TCO'd — but overflow is a recoverable panic

Only **self** tail calls are optimised (including, since 2026-07-05, self
tail calls inside a `receive ... after` body — previously those stacked a
real C frame per tick and killed long-lived heartbeat loops; swarm-code died
this way after 154s of a slow tool call). Two functions tail-calling each
other (the natural state-machine shape) still consume a C stack frame per
hop. See [REVIEW_FABLE_2026-06.md](REVIEW_FABLE_2026-06.md), O2.

Since 2026-07-05 the failure mode is contained: every generated function
entry runs a red-zone stack guard (`sw_stack_low()`, 32KB margin), so a
would-be overflow raises a NORMAL per-process panic — EXIT propagates to
links/monitors, supervisors restart the actor, and the OS process survives.
A fiber that blows past the red zone inside pure C (no sw frame in between)
still hits the guard page, but the crash handler now recognises that on
both SIGSEGV (Linux) and SIGBUS (macOS maps guard-page hits to
KERN_PROTECTION_FAILURE → SIGBUS) and prints the stack-overflow banner
instead of a generic crash. Gate: `tests/sw/test_stack_overflow.sw`.

**Impact:** mutually recursive FSMs panic (recoverably) at depth instead of
killing the binary. **Workaround for the depth itself:** keep the loop in
one function and dispatch on an argument (`fun fsm(state, n) { case state
{ ... } }`), or raise the per-process stack with `SW_PROC_STACK` (bytes,
`k`/`m` suffixes; e.g. `SW_PROC_STACK=1m`).

### No static type checking

`sw` is dynamically typed by design — there is no compile-time type checking, and
arity is checked only for calls to module functions. Names ARE checked: an
identifier bound nowhere is rejected by `swc build`, `swc run` and `swc test`
alike (see "Recently cleared"). Map keys are not: `map_get(m, 'nmae')` is a
runtime `nil`.

## Recently cleared

### The interpreter had no tail-call optimisation (cleared 2026-09-24)

Every interpreted call recursed on the C stack, so any loop longer than a few
hundred iterations died under `swc run` / `swc test`. Tail calls to user functions
(including mutual recursion and receive-loop servers) now run in place. Gate:
`tests/sw/run/test_interp_tco.sw`.

### Idiomatic list and value code was quadratic or worse (cleared 2026-09-24)

`tl()`, `[h | t]` patterns, `list_append` and cons copied the whole list; the
turn checkpoint deep-copied loop state as a tree (exponential on shared
substructure) and in full every time; every int, nil and boolean was a fresh
allocation. Summing a 100K list took 162s; it takes 0.1s. Gate:
`tests/sw/test_value_scaling.sw`.

### Typo'd variables compiled to atoms (cleared 2026-09-24)

`print(totl)` used to compile to `print(:totl)` and interpret as `print(nil)`. A
shared static pass (`sw_resolve_module`) now reports every name that no scope binds,
with a did-you-mean, before either backend runs. Gate:
`tests/sw/compile_fail/undefined_names.sw`.

### HTTP client builtins pinned their scheduler thread (cleared 2026-09-24)

`http_get`/`http_request`/`http_post`/`exec_argv` spawned curl and waited for it on the
scheduler thread, capping in-flight calls at the scheduler count and deadlocking a
program that called its own in-VM server under `SW_SCHEDULERS=1`. They now run on the
offload pool while the caller parks. Gate: `tests/sw/test_http_offload.sw`.

### HTTP server dropped requests and leaked fds (cleared 2026-09-24)

A second `sw_io_init` from `http_listen` started a second IO thread on the same sockets
(data could overtake its connection's accept and be dropped), and peer-closed sockets were
never closed. Gate: `tests/sw/test_http_fd_leak.sw`.


### Spin-gated scheduler deadlock — root-caused: Dekker StoreLoad bug in the receive handshake

Cleared 2026-06-10. The ~15%-incidence total deadlock under depth-1
cross-scheduler ping-pong (both fibers parked WAITING, zero enqueues
forever) was a one-line memory-ordering bug, not a data race:
`sw_receive_any`'s fast path did

```c
atomic_store_explicit(&waiting, 1, memory_order_release);
sig = atomic_load_explicit(&sig_head, memory_order_acquire);   // different var!
```

— Dekker's pattern. The store sat in the receiver's store buffer past
the load (x86 StoreLoad reordering; C11 gives release→acquire on
DIFFERENT objects no ordering at all), so the receiver saw a pre-push
`sig_head` and parked, while the sender's wake-xchg read `waiting==0`
from memory and skipped the enqueue. Both sides lost; the message sat
in the mailbox forever. The wedge autopsy (`SW_SCHED_TRACE`) showed the
exact corpse: parked, `waiting=1`, `sig_head!=NULL`. TSan was silent
throughout — every access was atomic; the bug was pure ordering. The
three sibling receive paths survived on x86 only because
`mailbox_drain`'s locked xchg is a full fence — NOT a guarantee on
arm64 (the macOS daily driver), so they were latent there too.

Fix: every `waiting` participant is seq_cst (all four stores, the
sig_head probe load, `mailbox_wake`'s exchange) — total-order
correctness on every architecture, nanosecond cost on cold paths.
Verified: **0/300** wedge-hunt runs post-fix (~45 expected at the old
incidence), full battery + matrix green, tsan-gate clean. The idle-loop
spin (`SW_SPIN_US`) defaults ON again: cross-scheduler ping-pong
measured **58.4 → 3.0 µs/rt (19×)**. Regression gate:
`tests/stress/spin_wedge_hunt.sh`.

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
