# SwarmRT changelog

Recent commits, newest first. Strict format: date, headline, what changed, what unblocked.

---

## Current state — what's in the build

As of `927cb30` (2026-05-20) plus REPL builtin coverage + `eval/` benchmark, the `.sw` language has:

- **Core:** `module / fun / export / import`, `spawn / send / receive`, `case`, `if / else`, `try / catch`, pattern matching with guards.
- **Values:** int, float, string, atom (`'ok'`), tuple (`{...}`), list (`[...]`), map (`%{key: val}`), pid, nil, fun. `map_get` treats atom and string keys interchangeably; `++` works on lists too.
- **Concurrency:** lock-free MPSC mailboxes, selective receive, ETS for shared mutable state. Supervisors (one-for-one / one-for-all / rest-for-one) plus `link`, `unlink`, `monitor`, `demonitor`, `exit_proc`, `trap_exit` — full Erlang fault-tolerance surface from userland sw.
- **Built-in I/O:** HTTP (server + client + streaming), WebSocket client/server, Chrome DevTools (browser automation), files, JSON, base64, shell (+ `shell_sandboxed` for sandbox-exec / firejail isolation), bidirectional subprocesses (`subprocess_*`), SQLite (`db_open / db_exec / db_query`).
- **Ergonomics:** f-strings (`f"hi {name}"`), `format("hi {} count {}", n)`, `++` polymorphic, variadic `print`, `;` works in any block, C-reserved words legal as identifiers, `case` for top-level pattern dispatch.
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
- **Runtime:** programs exit when `main()` returns (Go-style). 100K+ concurrent processes per node, ~150ns context switch.

Sw test suite is **8 compiled files (110 assertions) + 1 interpreter file (16 assertions)** = 126 total. swarm-code is the canonical real-world consumer; rebuilds clean against every commit.

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
alias subparts of it. CI stress widened to 50 runs at 90% threshold
across both multi- and single-scheduler variants (the round-5 fix
closed the original race deterministically but a different race in
the message-send path still fires on Linux x86_64 spawn-storms;
single-scheduler reproduces, so the multi-only run wasn't catching
it). Round-4 audit cleanups: `pmap` accepts either arg order like
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
honoured by `sw_spawn_opts` when non-NULL. After the fix all 8 phase
test files pass 100% — phase 2 (GenServer/Supervisor), 3 (ETS, 15
tests), 4 (Agent/App/DynSup, 14 tests), 5 (StateMachine/PG, 12
tests), 6 (TCP, 6), 7 (hot reload, 5), 8 (GC, 5), 9 (distribution,
4). All wired into CI. README example now mirrors
`examples/counter.sw` verbatim so what you see is what `./counter`
prints (`Count: 8` + `Counter stopped at 8`).

What's still open: one spawn-storm race in the message-send path on
Linux x86_64 (single-scheduler reproduces; 80k spawn workload, ~16%
crash rate under stress). Heap-corruption-then-strdup-SIGSEGV
pattern, not the ctx-tear race. macOS arm64 + ASan doesn't
reproduce. Documented in KNOWN_ISSUES with the suspect (mailbox/atom
allocator interaction) and the next investigation step
(helgrind/TSan on Linux).

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
