# SwarmRT changelog

Recent commits, newest first. Strict format: date, headline, what changed, what unblocked.

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
