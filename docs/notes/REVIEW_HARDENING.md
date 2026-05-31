# Review-driven hardening (May 2026)

Between 2026-05-15 and 2026-05-21, SwarmRT went through six rounds of
external review — five from a Claude web agent, one from Codex. Each
round filed a markdown report against the latest commit; each fix
shipped as a single commit titled "Round N: …" so the reviewer's next
pass could verify against a clean SHA.

This doc consolidates what came out of it. Two reasons to read:

1. **As a user**, you want to know what shape the runtime is in
   today: what's been hammered on, what's verified end-to-end, and how
   unresolved items are tracked.

2. **As a contributor**, the bug stories are mostly small but the
   diagnoses ate a lot of cycles. The pattern of "Heisenbug that
   only fires on native Linux x86_64, never on macOS arm64 or
   emulated Docker" came up multiple times. The methodology notes
   below save a future contributor from repeating those mistakes.

No open runtime issue remains from these review rounds. New open items
belong in [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md) with a repro and current
hypothesis.

---

## The rounds at a glance

| Round | Headline issue | Fix kind |
|---|---|---|
| **R2** | `exit(1)` on any panic killed the runtime | Per-process panic via scheduler `longjmp` |
| **R2** | JSON-encoded distribution lost tuple-vs-list / atom-vs-string | Type-preserving binary marshal |
| **R3** | `spawn(fun() {...})` silently returned pid 0 | Generic lambda trampoline |
| **R3** | EXIT/DOWN reason arrived as `-1` instead of the panic message | Threaded reason string through signal |
| **R3** | High-process-count crash at ~62k spawns | First attempt: 1-slot deferred-free (failed) |
| **R4** | Distribution `self()` round-trip arrived as nil | `SW_VAL_REMOTE_PID` + `sw_send_dispatch` |
| **R4** | Same race as R3 | Heavier attempt: 64-slot ring (also failed) |
| **R5** | TCP framing dropped all but the first frame per read | Per-peer `rx_buf` + drain loop |
| **R5** | Same race | Per-slot generation counter + `ctx_lock` + new asm (worked for ctx-tear) |
| **R6** | Different race surfacing on Linux x86_64 spawn-storm | Message envelope leak plugged; CI widened; cleared May 29 |
| **R7** | Codex caught 7 phase tests failing while CI was green | `sw_spawn_link` scheduler pin actually pinning now |

---

## R2 — first deep audit

The runtime worked end-to-end but crashed on the first user mistake:
`hd([])` called `exit(1)` and took down every process on the node.
Distribution worked in the demo but didn't survive a real round-trip
(JSON lost types). The compiler accepted obvious typos with no
warning. CI was a single ubuntu image checking that `swc` could build
the README counter.

Shipped:

- **Per-process panic recovery.** Each scheduler keeps a `sigjmp_buf`
  and on `sw_process_panic()` we `siglongjmp` back to the scheduler
  loop, which marks the process EXITING and continues. The runtime no
  longer dies when one process does. Panics carry a `reason_str` that
  `trap_exit` handlers receive in `{'EXIT', from, reason}`.

- **Distribution serialisation.** Replaced JSON with a tagged binary
  format (`SW_MARSHAL_*`) that preserves tuples, atoms, lists, maps,
  pids, ints, floats, strings. Cross-arch is a future concern — for
  now both ends are assumed to share endianness/word-size. The wire
  is small (1 tag byte per value + length-prefixed payloads) and the
  parser caps recursion / collection size to keep malformed input
  bounded.

- **HTTP POST body delivery.** Off-by-one in the Content-Length
  framing — we were reading the headers correctly but the body was
  truncated by one byte for any POST larger than ~14 chars. Caught
  by an external user; trivial fix, embarrassing miss.

- **ETS enumeration.** `ets_list` returned `[]` and `ets_count`
  returned `-1` regardless of table contents. The implementation
  was scanning the wrong static (`_vets_tables` vs the actual
  `g_ets_tables`). Rewritten by a subagent in parallel with the
  other R2 work.

- **Scheduler count auto-detect.** Was hardcoded to 2. Now reads
  `SW_SCHEDULERS` env var, falls back to `sysconf(_SC_NPROCESSORS_ONLN)`.

- **Compiler diagnostics.** Compile-time arity check (sticky-flag
  pattern — first mismatch noted, compilation continues to find more,
  then errors); halt-on-unknown-function with Levenshtein
  did-you-mean suggestions (length-adaptive threshold: 1 for short
  names, 2 for medium, 3 for long).

---

## R3 — lifecycle

R2 fixed most user-visible bugs. R3 found two lifecycle bugs that
silently corrupted behaviour rather than crashing.

- **`spawn(fun() {...})` returned pid 0.** The codegen's `emit_spawn`
  recognised `spawn(worker())` (an `N_CALL` of a known module
  function) and registered a trampoline. It did NOT recognise
  `spawn(fun() { body })` (an `N_FUN` with no name), so the spawn
  fell through to the "spawn failed" fallback and quietly returned
  pid 0. Anyone using the natural lambda form got a no-op process.
  Fix: lambda branch in `emit_spawn` that wraps the closure as
  `SW_VAL_FUN` and hands off to a generic
  `_sw_lambda_spawn_trampoline` that calls `sw_val_apply`.

- **EXIT/DOWN reason was `-1`.** When a process panicked,
  `trap_exit` handlers received `{'EXIT', from, -1}` — the legacy
  integer reason, with no clue what actually went wrong. The panic
  message ("hd: list is empty", "panic: foo", etc.) was already
  formatted but discarded at the signal boundary. Added
  `sw_signal_t.reason_str` (heap-owned, strdup'd at delivery, freed
  at msg cleanup); `deliver_signal` carries it through; codegen
  `emit_receive` synthesises `_sig->reason_str` for the EXIT/DOWN
  tuple shape when present, falls back to the int otherwise.

Also in R3: first attempt at the arena slot-reuse race the round-2
report mentioned. The hypothesis was that destroying thread A's
process P while thread B's `sw_spawn` reused P's slot was tearing
the ctx struct mid-`sw_context_swap`. A 1-slot deferred-free per
scheduler ("hold the slot for one scheduler tick before returning to
the partition free list") should buy enough time for any in-flight
swap to drain. Round 4 measurements showed it didn't — the deferral
interval was the same order as the race window, so slots could be
re-released during the race.

---

## R4 — distribution second pass + heavier race attempt

The R2 binary-marshal got distribution working in the trivial
direction (alpha → beta) but not the reply: `from` arrived as nil
because the encoded pid had no node attribution, and the receiver
had no way to know "this pid lives on alpha." Round 4 closed it:

- **`SW_VAL_REMOTE_PID`** new value type, `{char *node, uint64_t id}`.
  Marshal writes pid id only; receiver reconstructs as remote-pid
  tagged with the sending node's name (carried in the wire header).
- **`sw_send_dispatch(target, msg)`** wraps the codegen-emitted send.
  `SW_VAL_PID` routes through `sw_send_tagged` as before;
  `SW_VAL_REMOTE_PID` routes through `sw_node_send_pid` (marshals
  and writes to the peer's TCP conn).
- **Auto-peer-registration on incoming connections** so beta knows
  about alpha after alpha's first packet — alpha called
  `node_connect`, beta didn't, but `send(from, reply)` from beta
  needs to find alpha in the peer table.
- **`arena->next_pid` starts at 1.** Pid 0 was the "no pid" sentinel
  that `sw_find_by_pid` rejected. The very first spawned process
  (the wrapper around user `main`) used to get pid 0 and any remote
  reply to its `self()` vanished on lookup.

Verified end-to-end: `alpha: REPLY: hello-rpid` arriving back from a
beta that received `<rpid:alpha@127.0.0.1:1>` as the sender.

R4 also widened the deferred-free ring from 1 slot to 64 slots —
still heuristic, still the wrong shape. **R5 measurements showed it
actually regressed 40k/50k thresholds (74% fail where R2 was 100%
green)** because spreading the deferral across more slots increased
allocator pressure during spawn-storms. The race is event-bounded
(context-switch duration), not allocation-rate bounded — no ring
size fixes it deterministically.

---

## R5 — distribution framing + deterministic race fix

Two unrelated wins in one push.

**TCP framing (P0 for distribution).** `dist_handler`'s `PORT_DATA`
branch read the 4-byte length prefix, processed exactly one framed
message, then `free`d the whole receive buffer including any
subsequent frames the kernel coalesced into the same read. Anything
beyond `ping → pong` (registry lookups, RPC patterns, agents sending
state updates) sent multiple messages and arrived flaky — first
message through, the other 19 silently dropped. Worse, alpha's
`node_send` returned `'ok'` (TCP write succeeded), beta just never
received.

Fix: added `rx_buf` / `rx_len` / `rx_cap` to `sw_peer_t`,
`find_peer_by_conn_locked`, and a drain loop that consumes every
complete `[len][hdr][payload]` frame and leaves any partial frame
in the buffer for the next read. `PORT_ACCEPT` pre-registers a
placeholder peer keyed by conn so the very first frame has somewhere
to land; `handle_remote_data` promotes the placeholder to a named
peer once it parses `from_node`. Verified: 20 back-to-back messages
(was 1/20), 500 messages in 169ms (~3k RPS), 10k messages in 3.6s,
50KB single payload arriving intact via the split-read path.

**Arena race — the deterministic fix.** Ripped out the ring entirely.
Reasoning: a destroying thread holds a slot until reuse by an
allocating thread; the in-flight `sw_context_swap` on a *third*
thread is reading `proc->ctx` field-by-field. The race window is the
asm read window, not the wall-clock between free and re-allocate.

Standard ABA-defense pattern. Each slot gets:
- `_Atomic uint64_t generation` — bumped on every `process_init_arena`
- `sw_spinlock_t ctx_lock` — serialises ctx write vs ctx copy

`process_init_arena` takes `ctx_lock`, bumps `generation` (release),
writes ctx fields, releases the lock. The scheduler samples
`generation` at pick time. New helper `sw_safe_swap_into`:

```c
sw_spin_lock(&to->ctx_lock);
if (atomic_load_explicit(&to->generation, ACQUIRE) != expected_gen) {
    sw_spin_unlock(&to->ctx_lock);
    return -1;   // slot was reused — skip, pick another
}
sw_context_t local_ctx = to->ctx;     // stable copy under the lock
sw_spin_unlock(&to->ctx_lock);
sw_context_swap_from_copy(from, &local_ctx);   // asm reads local
```

A new asm symbol `sw_context_swap_from_copy(from, ctx_ptr)` is
identical to `sw_context_swap` except the restore side reads from a
caller-owned `sw_context_t *` (offset 0) instead of from
`proc->ctx` (offset `CTX_OFFSET`). The C wrapper holds the lock just
long enough to memcpy the ctx into a stack-local; the asm reads from
the stack-local. A concurrent `process_init_arena` on any other
thread can run freely; it can't tear our snapshot. Added for x86_64
SysV, x86_64 Windows, and ARM64.

`process_destroy` returns slots immediately again — no ring, no
deferred-free. Restores the 40k/50k thresholds that R4-B regressed.

CI gate: `make stress` runs the 80k spawn bench × 20 on
`ubuntu-24.04`. Workflow at
`.github/workflows/linux-quickstart.yml`.

---

## R6 — message-send race surfaces; envelope leak; audit cleanups

R5 closed the named ctx-tear race deterministically. Round 6
verification showed a **different** crash emerging under spawn-storm:
`sw_val_atom("done")` SIGSEGV'd in `strdup`, with a trampoline return
address of `0x0`. Classic heap-corruption-then-next-malloc pattern.

Single-scheduler reproduces (5/20 at 80k) — so it can't be the
multi-scheduler ctx race. Reviewer's `quietkids.sw` (80k spawns,
`1+1` in child, no sends) hits 16/20; the sends-version (with
`send(parent, 'done')` per child) hits 3/20. The send path is the
dominant additional contributor.

What round 6 shipped (defensive, not closing):

- **`sw_msg_release(sw_msg_t *)`** exposed as a public function;
  `emit_receive` calls it after each matched body runs. The receive
  codegen used to drop the `sw_msg_t` envelope entirely after a
  match — the per-thread `tls_msg_free` freelist stayed empty and
  `msg_alloc` always missed straight to `malloc`. Under spawn-storms
  this kept the glibc arena saturated and likely contributed to the
  strdup crash. The payload `sw_val_t` is left alive on purpose —
  pattern bindings (`from = msg->v.tuple.items[1]`) alias subparts
  of it, and the body's return value can itself be one of those
  bindings.

- **CI stress widened, then tightened.** R6 widened 20 -> 50 runs,
  threshold 18 -> 45 (90%), and added a `SW_SCHEDULERS=1` variant since
  single-sched reproduced the new race. The May 29 sushi retest later
  cleared the race and `make stress` is now strict by default: every
  configured run in both variants must complete.

R6 also closed the round-4 audit items:

- `pmap` accepts either arg order (was strict `pmap(fn, lst)`; docs
  showed `pmap(lst, fn)`) — now type-sniffs like `map` and `filter`.
- `map_has_key` reuses `sw_val_map_get` so the atom-vs-string
  fallback matches — `map_has_key(%{a:1}, "a")` now returns true
  (was false).
- `expect(nil, msg)` now panics. The literal `nil` lexes to atom
  `'nil'`, not `SW_VAL_NIL`, so the previous strict-type-check fell
  through and silently returned the atom. `expect` now treats atom
  `"nil"` as nil too (matches `sw_val_is_truthy`).

`pubsub_subscribe` / `pubsub_broadcast` verified working end-to-end
locally — reviewer's note about non-delivery couldn't be reproduced.

---

## R7 — Codex catches the CI gap

The Codex review (filed against `774ce9e`) confirmed everything from
round 6 worked, then dropped this:

> Some local runtime tests fail:
> - `test-phase4`: agent_concurrent (13/14)
> - `test-phase5`: pg_join_dispatch, pg_leave, pg_members, pg_multiple_groups (8/12)
> - `test-phase7`: multi_upgrade, dead_cleanup (3/5)
>
> CI mainly covers README build path, examples, `make test-sw`, stress.
> It does not appear to run all phase tests.

**Caught fair.** `make test-sw` runs only the `.sw` language suite.
The C-side phase tests had been failing locally for some time —
nobody noticed because nobody ran them and CI didn't gate on them.

Reproduced all seven failures locally; they share a pattern. Each
test spawns N children with `sw_spawn_link`, `usleep`s briefly, then
checks group membership / supervisor children / etc. Result is
consistently off by **one** — one child appears not to have run.

Root cause:

```c
// sw_spawn_link's "force child to a different scheduler" block:
sw_scheduler_t *save_sched = tls_scheduler;
if (save_sched && g_swarm->num_schedulers > 1) {
    uint32_t target;
    do {
        target = __sync_fetch_and_add(&g_swarm->next_sched, 1)
                 % g_swarm->num_schedulers;
    } while (target == save_sched->id);
    tls_scheduler = g_swarm->schedulers[target];   // override
}
sw_process_t *child = sw_spawn(func, arg);
tls_scheduler = save_sched;                         // restore
```

But `sw_spawn_opts` (the implementation `sw_spawn` calls) picks its
scheduler from the global `next_sched` round-robin counter and
**never reads `tls_scheduler`**. The entire override block was a
no-op. Children went via plain round-robin, occasionally landed on
the parent's scheduler, and stalled behind the parent's
`usleep`/blocking-receive. One stalled child → one missing member.

Fix: separate TLS slot `tls_spawn_override`. `sw_spawn_link` sets it
for the duration of the inner `sw_spawn` call; `sw_spawn_opts`
honours it when non-NULL, falls back to round-robin otherwise.
Clean separation — plain `sw_spawn` from anywhere else still
distributes via round-robin as before.

After the fix, 5× repeated runs of phase 2/3/4/5/6/7/8/9 all stay
100% green. All wired into CI — regression on any phase will turn
the workflow red.

Also from R7: README `count: 8` (the hand-written snippet) vs actual
`Count: 8 / Counter stopped at 8` (what `examples/counter.sw`
prints). README example now mirrors the example file verbatim.

---

## What this means today

Everything CI-gated, every push:

- README quickstart + a handful of example programs.
- 110 compiled-language assertions + 16 interpreter assertions
  (9 `.sw` test files).
- 9 C-side phase test files — **73 tests total, 100% green**.
- 100 stress runs (50 multi-scheduler + 50 single-scheduler), 80k
  spawns each, strict by default: every run must complete.

Open known runtime issues ([`KNOWN_ISSUES.md`](KNOWN_ISSUES.md)): none.

The previous spawn-storm race was cleared on May 29 with a native Linux
x86_64 run on `sushi`: 50/50 multi-scheduler and 50/50 single-scheduler
completed with zero crashes. Any future miss in `make stress` should be
treated as a regression, not an accepted flake.

What's NOT shipped but flagged by reviewers as future work:

- Security hardening of the shell-out paths (popen/system in MCP,
  studio builtins, coder). Sanitisation exists; the recommendation
  is `posix_spawn` + strict allowlists + command-injection regression
  tests.
- Module-level globals — real architectural change, not a bug fix.
- Deadlock detection for silent hangs.
- Fixnum / immediate-tag small ints (R2-#10, deferred by reviewer
  as not worth blocking on for now).

---

## Methodology notes for future contributors

A few patterns that repeated across rounds. Save the cycles next
time:

**Docker on Apple Silicon doesn't reproduce Linux x86_64 races.**
Docker Desktop runs x86_64 binaries under qemu user-mode emulation,
which serialises thread interleavings aggressively enough to hide
most thread-scheduling races. An early commit (`3a5e029`) claimed
"couldn't repro after 50 stress runs in Docker" — that conclusion
was wrong and the credibility wobble took a round to recover.
**Run on native amd64.** GitHub Actions `ubuntu-24.04` runners are
native amd64; that's why the CI gate works.

**`valgrind` also serialises threads.** Useful for memory errors,
not useful for race reproduction. The round-2 valgrind check was at
N=1k, well below the ~62k race threshold; it told us nothing about
the race.

**macOS libmalloc is forgiving; glibc malloc is strict.** A
write-after-free on macOS often silently leaks; the same code on
Linux corrupts the arena and the next `malloc` SIGSEGV's in odd
places (often a totally unrelated `strdup`). When a backtrace shows
a crash inside a standard library function, suspect the heap state,
not that function.

**Heisenbugs that vanish under printf are timing-dependent races,
not "the printf fixed it."** Phase 3's `ets_owner_exit` flaked
between R6 and R7 — adding a debug printf made it pass. The
underlying cause was almost certainly the same scheduler-pinning
bug R7 ended up fixing; the printf shifted timing past the race
window.

**`MEMORY.md`-style entries in `~/.fieldtheory/` and external
review reports are load-bearing.** When the reviewer reports
verifiable measurements ("9/50 at 80k spawns"), trust them over
your own "works on my machine" — especially when you only have a
machine the race doesn't fire on.
