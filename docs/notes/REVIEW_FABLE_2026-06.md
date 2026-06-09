# Round 7 — full-stack audit (June 2026, Claude Fable)

A seventh external review round, in the tradition of
[REVIEW_HARDENING.md](REVIEW_HARDENING.md): clone fresh, build from
scratch, run every CI gate, write `sw` cold as a first-time user, read
the runtime internals, and file what was found. Three bugs were fixed in
this round (one of them the gate-breaking one); the rest are open items
with repros, ordered into an upgrade path at the end.

**Environment:** Linux x86_64 (kernel 6.18, 4 vCPU Xeon, 16 GB), gcc,
container with default `vm.max_map_count` = 65530. Absolute timings
below carry that caveat — this is a shared/virtualized box (a static C
binary takes ~3 ms to fork+exec here), so treat them as relative, not as
hardware truth.

---

## Verdict in one paragraph

The core thesis holds. The BEAM-shaped semantics that matter — crash
isolation, `monitor`/`DOWN` with real panic reasons, `trap_exit`,
supervisor restart, selective receive, flat-memory long-lived loops —
all verified end-to-end from `sw` source, and the Ownership v2 memory
gates pass with genuinely flat slopes. The honesty culture (KNOWN_ISSUES,
doc tripwire, RED-gate-first commits) is real and rare. But this round
also found that the flagship reliability gate (`make stress`,
single-scheduler variant) failed **50/50** on a default-tuned Linux box,
because compiled tail loops could not be preempted — a scheduler-fairness
hole that CI never saw since GitHub/Ubuntu-24.04 hosts raise
`vm.max_map_count`. That and a C-level UB bug in case-arm codegen (the
natural Erlang idiom `case n { n when n > 0 -> ... }` returned wrong
answers or SIGSEGV'd) are fixed in this round. The remaining gap between
pitch and product is concentrated in one place: **compiled/interpreter
divergences** and **silent failure modes**, both of which are poison for
the LLM-writes-it-first-try story.

---

## Fixed in this round

### F1 (P0) — case-arm self-shadow emitted C undefined behavior

`emit_case` bound arm pattern variables straight off the subject's C
identifier. When an arm re-binds the subject's own name — idiomatic
Erlang, and the first thing this reviewer wrote cold:

```sw
fun classify(n) {
    case n {
        0 -> 'zero'
        n when n > 100 -> 'big'     # bound as: sw_val_t *n = n;  ← UB
        n when n > 0   -> 'small'
        _ -> 'negative'
    }
}
```

the generated C was `sw_val_t *n = n;` — the new `n` shadows at its own
declarator, so the arm-local read indeterminate stack memory.
`classify(5)` returned `'negative'` on a calm stack and SIGSEGV'd inside
`_op_cmp` on a dirty one (reproduced both ways). The interpreter was
always correct, so this was also a compiled/interpreter divergence.
Tuple, cons, and map sub-binds (`n->v.tuple.items[0]`) carried the same
hazard.

**Fix:** snapshot the subject into a fresh compiler temp before any arm
binds (one pointer copy per `case`); `with` desugars to `case`, so it is
covered. **Test:** `tests/sw/test_case_shadow_scrutinee.sw` (7
assertions, compiled + interpreter).

### F2 (P0) — compiled tail loops had no yield points; spawn storms died by VMA exhaustion

The README claims reduction-counted preemption, and `sw_check_reds()` /
the 1 ms SIGALRM force-yield exist in the runtime — but generated code
never called them. A compiled self-tail-call loop (`_tail:` … `goto
_tail`) was unpreemptible. Consequence chain, fully verified here:

1. `SW_SCHEDULERS=1` + the 80K spawn storm (`make stress`): the parent
   loop never yields, so all 80K children stay queued behind it, every
   child's 128 KB stack live simultaneously;
2. each stack is its own `mmap` + guard page ≈ 2 VMAs → ~160K VMAs
   needed, over Linux's default `vm.max_map_count` 65530 (peak observed:
   60,360 at a 100 ms sampling interval before death);
3. `mmap` and then glibc `calloc` start returning NULL; value
   constructors wrote through the NULL → `SIGSEGV at address (nil)`.

Result on this host: **single-sched stress 0/50** before the fix.
Why six review rounds and CI never caught it: Ubuntu 24.04 (the May-29
"sushi" clearance box) defaults `vm.max_map_count` to ~1M, and GitHub's
Azure runners raise it too — the gate's verdict was silently a function
of a host sysctl.

**Fix:** emit `if (sw_check_reds()) sw_yield();` at the self-tail-call
backedge — the language's only unbounded loop construct. One TLS
decrement per turn, yields every `SWARM_CONTEXT_REDS` (2000) turns; both
calls are no-ops on the interpreter path. This also makes the 1 ms
preemption timer actually bite on compiled loops, i.e. the README's
"reduction-counted preemption" is now true for compiled code.
**Result:** single-sched stress **50/50** (was 0/50); full `make stress`
passes both variants; storm completes in ~355 ms.

### F3 (P1) — allocation failure SIGSEGV'd instead of failing loud

`val_alloc`/`val_strdup` fell back to `calloc`/`strdup` with no NULL
check, and every `sw_val_*` constructor writes through the result
unchecked. Under any real OOM (or the VMA exhaustion above) the death
was a bare segfault with no diagnosis. Now: a loud panic naming the
allocation size and the most likely cause (`vm.max_map_count` / live
process count) before abort. Memory exhaustion is still fatal — but it
is now *diagnosable* fatal, which is what the "loud failures" philosophy
promises.

---

## Verified claims (credit where due)

| Claim | Verdict |
|---|---|
| Quickstart 60s: `make swc libswarmrt` → counter.sw | ✓ exactly as documented |
| Crash isolation: a spawned process's panic kills only it | ✓ main survives; no node death |
| `monitor` → `{'DOWN', ref, 'process', pid, reason}` with the real panic message | ✓ |
| `link` + `trap_exit` → `{'EXIT', from, reason}` | ✓ |
| `supervise('one_for_one', …)` restarts a panicked child; `whereis` re-resolves | ✓ |
| Self-tail recursion is flat (1M deep) | ✓ |
| 100K+ processes per node | ✓ post-F2 (was SIGSEGV on this host) |
| Ownership v2 flat-memory slopes (`make gc-slope`) | ✓ PASS, 0 MB growth on all four probes |
| GC copy-on-escape under ASAN (`make gc-stress`) | ✓ PROBE_OK |
| Test suite: 54 files / 489 assertions; phases 2–10; doc tripwire 28/28 | ✓ all green (post-fix) |
| REPL, LSP exist; multi-module import; `swc emit`/`test`/`run` | ✓ smoke-tested |
| Eval table in README matches `eval/results/results.md` | ✓ |
| `string_length` = byte length | ✓ documented honestly (see L6) |

Measured here (container caveat applies): full
spawn+send+receive+exit cycle ≈ 2.6–4.6 µs; same-scheduler ping-pong
round trip ≈ **920 ns** (two sends + two selective receives + two context
switches); startup of a hello binary ≈ 105–145 ms wall (vs ~3 ms for a
static C binary — see O5).

---

## Open findings

### O1 (P1) — `try/catch` semantics diverge: interpreter catches panics, compiled code doesn't

```sw
r = try { hd([]) } catch e { "caught" }
```

`swc run`: `r = "caught"`. Compiled: the panic propagates and the
process dies. The docs side with the compiled behavior ("panics cannot
be caught"), so the interpreter is the one violating spec — but either
way the same program means two different things on the two paths, which
is the exact class of drift the repl test suite exists to prevent.
Decide one semantics and gate it. Recommendation: make panics catchable
*at the process boundary only* (they already are, via `trap_exit`/
`monitor`) and fix the interpreter to let them kill the (virtual)
process; OR adopt Erlang's stance that everything is catchable. The
current split is the worst of both.

### O2 (P1) — mutual tail recursion is not TCO'd; overflow is a silent SIGSEGV

`docs/SW_LANGUAGE.md` §"Recursion is the loop construct" says *"Tail
calls are detected and optimised by the codegen, so unbounded tail
recursion doesn't blow the stack."* Only **self** tail calls are
optimised (`is_self_call`). Two state functions tail-calling each other —
the canonical actor FSM shape — eat a C stack frame per hop and kill the
128 KB fiber stack at depth ~10⁴–10⁵:

```sw
fun is_even(n) { if (n == 0) { 'true' } else { is_odd(n - 1) } }
fun is_odd(n)  { if (n == 0) { 'false' } else { is_even(n - 1) } }
# is_even(100000) → SIGSEGV, and the crash banner does not even print
```

Three sub-items: (a) doc overclaim — say "self-tail-calls" until it's
true generally; (b) the guard-page SIGSEGV handler apparently runs on
the overflowed fiber stack, so even the crash banner is lost — install
`sigaltstack` and print "stack overflow in process N (deep non-self
recursion?)"; (c) longer term, a trampoline or mutual-recursion grouping
in codegen would make the doc claim true.

### O3 (P2) — spawn failure is silent: a dud pid, then nothing

Generated spawn handles `sw_spawn_owned` returning NULL by freeing the
region — then continues into `sw_val_pid(NULL)` and carries on with a
pid value wrapping a null process. `send` to it goes nowhere. At the
process ceiling (`SW_MAX_PROCS`) a program degrades into silent message
loss. Should be loud: panic, or return a value `spawn`'s caller can
check, and say which in the docs.

### O4 (P2) — cross-scheduler wakeup dominates chatty workloads (58× here)

Ping-pong round trip: 920 ns when both processes share a scheduler;
~53 µs when they sit on different schedulers (every message finds the
peer's scheduler idle → mutex + condvar signal + OS wake latency, paid
twice per round trip). Numbers are container-inflated, but the shape is
architectural. Options worth measuring: brief spin-before-sleep in the
scheduler idle loop; spawn-time affinity (start a child on its parent's
scheduler — cheap and BEAM-ish); batching wakes. For agent swarms doing
request/reply with a coordinator, this is the number that matters.

### O5 (P2) — startup: ~105–145 ms observed vs "<10 ms" in the README

A C control binary forks+execs in ~3 ms on the same host, so ~100+ ms is
runtime boot, not the container's exec overhead (SW_MAX_PROCS=1024
shaves ~35 ms as documented). Maybe it's <10 ms on the daily-driver Mac;
on this Linux box it is not. Worth profiling arena init + scheduler
spin-up on Linux before re-stating the claim, or qualifying it.

### O6 (P3) — diagnostics polish for the LLM story

- Unknown-function errors **inside an f-string interpolation** report
  line 1, not the call's line.
- Diagnostics print a fabricated path (`src/Stress1.sw`) when the file
  isn't under `src/` — confusing for `swc build /tmp/foo.sw`.
- A miss on a **module-qualified** call (`Std.map(...)`) skips the
  "did you mean" pass entirely and surfaces as raw cc
  implicit-declaration warnings + a linker error
  (`undefined reference to 'Std_join'`). Module-function misses should
  get the same Levenshtein treatment as bare builtins, at codegen time.

### O7 (P3) — grammar/stdlib paper cuts an LLM will hit

1. **No destructuring assignment**: `{sq, _} = hd(results)` is a parse
   error; patterns only bind in `receive`/`case`/`with` arms. Elixir
   muscle memory writes the former. Either support `=`-patterns or add a
   "did you mean `case`" parse hint.
2. **`with`/`else` accepts exactly one arm** — multi-arm `else` (legal
   in Elixir) is a parse error at the second arm.
3. **`after MS { body }`** is brace-shaped while every arm above it is
   `pat -> body` — Erlang's `after MS ->` will be emitted by models. The
   parser could accept both.
4. **Namespace split**: `map`/`filter`/`reduce`/`each` are global
   builtins, the rest of the list toolkit lives in `Std`; there's no
   `join` anywhere (`Std.intersperse` + concat is the path). Models
   guess `Std.map` and `string_join` — both die. Consider `Std.map` /
   `Std.filter` aliases and a real `join`.
5. **No codepoint-aware string ops**: `string_length` is bytes
   (documented); agent code slicing UTF-8 text will corrupt multi-byte
   runes. A `string_chars`/codepoint-length builtin would close it.

### O8 (P3) — gate portability

`make stress`'s verdict depended on the host's `vm.max_map_count`
(masked by raised defaults on CI and Ubuntu 24.04 — see F2). Post-F2
this specific dependence is gone, but the lesson generalizes: the gate
scripts should `sysctl`-print the limits they're sensitive to, and
`gc-stress`/`gc-slope` should fail with "install X" the way gc-slope
already does for GNU time (gc-stress needs `libclang-rt-*-dev` for ASAN;
worth a line in CONTRIBUTING).

---

## Language design assessment

What's genuinely right: one concurrency story (`spawn`/`send`/`receive`,
no async coloring), braces + newline-or-semicolon (no indentation traps),
atoms/tuples/maps with literal syntax, guards, f-strings, the pipe, and
a runtime whose failure semantics (DOWN/EXIT reasons carrying real panic
messages) are *better* than several production actor systems. The eval
harness measuring first-try LLM correctness as a regression metric is a
legitimately novel piece of language-engineering process.

The strategic risk is **two execution paths with three observable
semantic gaps found in one session** (guards-UB now fixed; try/catch
O1; receive-timeout already in KNOWN_ISSUES). Every divergence converts
the language's main selling point — "the model's first try runs" — into
"the model's first try runs *in the REPL*". The interpreter and codegen
sharing builtins but not semantics is the deepest structural debt in the
project. A conformance suite that runs **every** `tests/sw/repl/*.sw`
through *both* paths and diffs stdout would catch the whole class
mechanically; it's the single highest-leverage CI addition available.

---

## Upgrade path (ordered)

**Now (this round):** F1–F3 shipped — case-shadow UB, tail-loop
preemption (stress gate 0/50 → 50/50), loud OOM. ✔

**Next (correctness, ~days):**
1. O1 — pick one try/catch-vs-panic semantics, fix the loser, add the
   dual-path conformance runner (compile *and* interpret every repl
   test, diff stdout) to `make test-sw`.
2. O2(b) — `sigaltstack` + honest stack-overflow message; doc fix to
   "self-tail-calls".
3. O3 — loud spawn failure.

**Then (LLM-ergonomics, ~a week):**
4. O6 — f-string line attribution, real paths, did-you-mean for
   module-qualified calls.
5. O7 — destructuring-assignment (or parse hint), multi-arm `else`,
   `after MS ->` tolerance, `Std.map`/`Std.filter`/`join`, codepoint
   string ops. Re-run the eval suite after; these five are plausibly
   worth several points on the non-frontier models.

**Later (performance/architecture):**
6. O4 — idle-loop spin-before-sleep + spawn-on-parent's-scheduler
   affinity; benchmark chatty-pair throughput before/after.
7. O5 — profile Linux boot; qualify or restore the <10 ms claim.
8. O2(c) — general TCO (trampoline or SCC-grouped dispatch loops in
   codegen) to make "tail calls are optimised" unconditionally true.
9. Consider pooling fiber stacks inside one large mapping (the PCB arena
   pattern) so live-process count never multiplies VMA count — removes
   the entire `vm.max_map_count` failure class rather than just
   preempting around it.

---

*Filed by the Round-7 reviewer (Claude, June 2026), against
`6f54390` + this round's fixes. Repro programs for every open finding
are inline above; all are <30 lines.*

---

## Round 7 continuation — status update (same reviewer, later the same day)

Rebased onto the Phase-1 ownership work (`e1a7713`) and executed the
"Next" tier of the upgrade path. Status of the open findings:

| Finding | Status |
|---|---|
| O1 try/catch divergence | **FIXED** — unified error model: `error()` unwinds the full dynamic extent to the nearest catch on both paths (compiled: per-process setjmp/longjmp chain); panics uncatchable on both; `error()` outside try silent on both. |
| O2(b) silent stack-overflow death | **FIXED** — SA_ONSTACK + per-thread sigaltstack; guard-page hits report "stack overflow in process #N" with the mutual-recursion explanation. O2(a) doc fix shipped earlier; O2(c) general TCO still future. |
| O3 silent spawn failure | **FIXED** — panics "process table full — raise SW_MAX_PROCS". |
| O6 diagnostics | **PARTIAL** — f-string interpolations now carry their real line. Fabricated `src/<Mod>.sw` paths and module-qualified did-you-mean still open. |
| O7 grammar/stdlib paper cuts | **MOSTLY FIXED** — tuple-destructuring assignment (`{'ok', v} = fetch()`, literal positions assert-match); multi-arm `with`/`else` with guards; `after MS ->` accepted; `Std.map`/`Std.filter`/`Std.join` exist (and the codegen guard that rejected them is relaxed). Still open: codepoint-aware string ops, list/nested destructuring. |
| O4 cross-scheduler wake cost | **FIXED** — bounded spin-before-park in the scheduler idle loop (`SW_SPIN_US`, default 30µs, 0 disables; producer pays nothing — `idle` stays unset while spinning). Measured on the original repro: 58.4 → 4.5 µs per cross-scheduler ping-pong round trip (13×); spawn cycles −26%; same-sched unchanged at ~920ns. |
| O6 diagnostics (remainder) | **FIXED** — real source paths registered by swc (diagnostics + #line use the actual file); module-qualified calls validate with did-you-mean ("module Std has no function 'mpa' — did you mean 'Std.map'?"). |
| O7 codepoint strings | **FIXED** — `string_chars(s)` (UTF-8 codepoint split, both paths, conform-gated). |
| O5 startup time, O8 gate portability docs | open (unchanged). |

**And the structural fix from the assessment section is in:** the
dual-path conformance gate (`tests/sw/run_conform.sh` + `tests/sw/conform/`,
wired into `make test-sw`). On its first run it caught four additional
real divergences beyond O1 — interpreter pipe was a no-op for most
builtins, piped `reduce` returned its init value compiled, compiled
`expect()` passed `'false'` through, and a panic inside an f-string
interpolation was laundered into the string `"nil"` by the interpreter.
All fixed and held closed by the gate.
