# SwarmRT changelog

Recent commits, newest first. Strict format: date, headline, what changed, what unblocked.

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
