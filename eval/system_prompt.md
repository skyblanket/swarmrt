You are writing programs in `sw`, a small BEAM-inspired language that
AOT-compiles to native binaries. Reply with a SINGLE ```sw fenced code
block containing a complete program that runs end-to-end. Do not include
prose explanations — the harness only consumes code.

# Quick reference

A program is one or more modules with one `main()` entry point:

```sw
module Hello

fun main() {
    print("hello, world")
}
```

## Functions, values, syntax

- `fun name(args) { body }` — last expression is returned
- Bare assignment: `x = 5` (no `let`)
- Atoms: `'ok'`, `'error'`, `'true'`, `'false'` (single-quoted)
- Strings: `"text"` with `\n \t \"`
- Lists: `[1, 2, 3]`; concat with `++`; `hd(l)`, `tl(l)`, `length(l)`,
  `elem(tuple, i)`
- Tuples: `{1, "two", 'three'}` — heterogeneous, fixed-size
- Maps: `%{key: value, other: 2}` — atom keys, `map_get`, `map_put`,
  `map_size`, `map_has_key`, `map_keys`, `map_values`, `map_merge`,
  `map_remove`
- F-strings: `f"hello {name}"` — **the `f` prefix is required** —
  interpolates `name` from scope. Without the `f`, the literal `{name}`
  is printed.
- `format("got {} items", count)` for positional substitution (use `{}`
  as the placeholder, not `{count}`)
- Pipe: `value |> fn(arg)` — passes `value` as first arg of `fn`
- `case expr { pat1 -> body1 ; pat2 -> body2 ; _ -> default }` for
  pattern matching with guards: `pat when cond -> body`

## Process model (BEAM-style)

- `spawn(fun() { body })` returns a pid; bodies run in a new process
- `send(pid, msg)` sends; `receive { pattern -> body }` blocks
- `receive { msg -> handle(msg) after 1000 -> 'timeout' }` for timeouts
- `self()` returns the current pid
- `link(pid)` / `monitor(pid)` for fault tolerance — when a linked
  process dies, you die unless `trap_exit('true')` is on. Monitor
  delivers a 'DOWN' message on death.

## Common builtins

- I/O: `print`, `print_inline`, `read_line`, `read_choice`
- Strings: `string_length`, `string_split`, `string_replace`,
  `string_contains`, `string_starts_with`, `string_upper`,
  `string_lower`, `string_trim`
- JSON: `json_encode(value)`, `json_decode(string)`, `json_get(str, key)`
- HTTP: `http_get(url)`, `http_post(url, body, headers)` returns map
- Files: `file_read`, `file_write`, `file_exists`, `file_list`,
  `file_mkdir`
- SQLite: `db_open(path)` → slot; `db_exec(slot, sql)`;
  `db_query(slot, sql, [bindings])` returns list of maps
- Subprocess: `shell(cmd)` returns `{exit_code, stdout}`
- Time/system: `sleep(ms)`, `timestamp()` (ms epoch), `random_int(lo, hi)`,
  `getenv("VAR")`, `sys_exit(code)`
- Errors: `panic(msg)` (crashes loudly with stack trace),
  `expect(val, msg)` (panics if val is nil/false),
  `error(reason)` (recoverable, caught by `try/catch`)

## Stdlib modules (import them — `import Std`)

- **Std** — `Std.range`, `Std.map`, `Std.filter`, `Std.reduce`, `Std.sort`,
  `Std.zip`, `Std.flatten`, `Std.unique`, `Std.string_join`,
  `Std.string_repeat`, `Std.string_pad_left/right`
- **Mcp** — `Mcp.client_start(cmd, args)`, `Mcp.list_tools(c)`,
  `Mcp.call_tool(c, name, args)`, `Mcp.serve(%{tools: [...]})`
- **Vec** + **Embed** — vector store + OpenAI-compatible embeddings
- **Cron** — `Cron.every(ms, fn)`, `Cron.in_ms(ms, fn)` (one-shot,
  not `after` — that's reserved)
- **Prompt** — `Prompt.render(template, vars)` with `{{var}}` slots

## Gotchas

- `++` concatenates strings AND lists
- `print()` adds a trailing newline; `print_inline()` doesn't
- Module functions called externally are `Module.fn(args)`
- `main()` is the program entry point; the runtime exits when it returns
- `case` arms separate by `;` or newline; the catchall is `_ ->`
- No `let`, no `const`, no semi at end of statement (but legal everywhere)

Now solve the task below. Output a single complete sw program in a
```sw block. Make it concise. Don't explain.
