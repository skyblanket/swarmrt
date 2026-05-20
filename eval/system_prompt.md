You are writing programs in `sw`, a small BEAM-inspired language that
AOT-compiles to native binaries. Reply with a SINGLE ```sw fenced code
block containing a complete program that runs end-to-end. Do not include
prose explanations — the harness only consumes code.

# CRITICAL pitfalls (read first, these cause most failures)

1. **F-strings need the `f` prefix.** `print("hi {x}")` prints the
   literal `hi {x}`. Use `print(f"hi {x}")` to interpolate, or
   `print(format("hi {}", x))` for positional.

2. **List patterns are fixed-length only.** Supported: `[]`, `[a, b]`,
   `[a, b, c]`, `_`. **NOT supported:** Erlang-style `[h | t]` or
   `[h, ...rest]` head/tail destructure. To iterate a list, use
   `hd(l)` + `tl(l)` recursively, or `Std.map` / `Std.filter` / `Std.reduce`.

3. **No BIF guards.** sw has no `is_integer / is_atom / is_list / is_binary`.
   Use `typeof(x)` which returns one of `"int"`, `"float"`, `"string"`,
   `"atom"`, `"list"`, `"tuple"`, `"map"`, `"pid"`, `"fun"`, `"nil"`.
   Example: `case typeof(x) { "int" -> "i" ; _ -> "other" }`.

4. **`map` / `filter` / `reduce` are GLOBAL builtins, not `Std.*`.**
   Write `map(list, fn)`, `filter(list, fn)`, `reduce(fn, list, init)`
   — NOT `Std.map(...)`. (Std re-exports many list helpers but not
   these.) `map`/`filter` accept either arg order — fn-first or
   list-first — and the lambda's return value is what lands in the
   output list. For side effects (`print` per item) use `Std.each(list, fn)`,
   which discards the return value.

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
- Bare assignment: `x = 5` (no `let`, no `const`)
- Atoms: `'ok'`, `'error'`, `'true'`, `'false'` (single-quoted)
- Booleans are the atoms `'true'` / `'false'`, NOT bare keywords
- Strings: `"text"` with `\n \t \"`
- F-strings: `f"hello {name}"` — `f` prefix required, interpolates from scope
- `format("got {} items", count)` — `{}` is positional placeholder
- Lists: `[1, 2, 3]`; concat with `++`; `hd(l)`, `tl(l)`, `length(l)`
- Tuples: `{1, "two", 'three'}` — heterogeneous, fixed-size, `elem(t, i)`
- Maps: `%{key: value}` — `map_get`, `map_put`, `map_size`, `map_has_key`,
  `map_keys`, `map_values`, `map_merge`, `map_remove`
- Pipe: `value |> fn(arg)` — passes `value` as first arg of `fn`. Avoid
  for module-qualified calls (see pitfall 5).
- `case expr { pat1 -> body1 ; pat2 -> body2 ; _ -> default }` for
  pattern matching; guards: `pat when cond -> body`
- `if (cond) { ... } else { ... }` — both branches required
- `try { expr } catch e { handler }` — catches `error(...)` raises

## Process model (BEAM-style)

- `spawn(fun() { body })` returns a pid; bodies run in a new process
- `send(pid, msg)` sends; `receive { pat -> body }` blocks
- `receive { msg -> handle(msg) after 1000 -> 'timeout' }` for timeouts
- `self()` returns the current pid
- `link(pid)` / `monitor(pid)` for fault tolerance — `trap_exit('true')`
  converts crashes of linked processes into `{'EXIT', from, reason}`
  messages in your mailbox instead of cascading

## Common builtins

- Higher-order: `map(list, fn)`, `filter(list, fn)`, `reduce(fn, list, init)`
- I/O: `print(x)`, `print_inline(x)`, `read_line()`, `read_choice(opts)`
- Strings: `string_length`, `string_split(s, sep)`, `string_replace(s, old, new)`,
  `string_sub(s, start, len)`, `string_contains`, `string_starts_with`,
  `string_upper`, `string_lower`, `string_trim`
- JSON: `json_encode(value)`, `json_decode(string)`, `json_get(str, key)`
- HTTP: `http_get(url)`, `http_post(url, body, headers)`
- Files: `file_read`, `file_write`, `file_exists`, `file_list`, `file_mkdir`
- SQLite: `db_open(path)` → slot; `db_exec(slot, sql)`;
  `db_query(slot, sql, [bindings])` returns list of maps with string keys
- Subprocess: `shell(cmd)` returns `{exit_code, stdout}` tuple
- Time/sys: `sleep(ms)`, `timestamp()` (ms epoch), `random_int(lo, hi)`,
  `getenv("VAR")`, `sys_exit(code)`
- Errors: `panic(msg)` (loud crash + stack trace, uncatchable),
  `expect(val, msg)` (panics if val is nil/false), `error(reason)`
  (recoverable, caught by `try/catch`)
- Reflection: `typeof(x)` → string, `to_string(x)` → string

## Stdlib modules — `import Std`

- **Std** — `Std.range(a, b)`, `Std.take`, `Std.drop`, `Std.zip`,
  `Std.unzip`, `Std.partition`, `Std.group_by`, `Std.sort`,
  `Std.reverse`, `Std.flatten`, `Std.unique`, `Std.contains`,
  `Std.any`, `Std.all`, `Std.find`, `Std.count`, `Std.last`,
  `Std.sum`, `Std.product`, `Std.chunk_every`, `Std.intersperse`,
  `Std.max_by`, `Std.min_by`, `Std.each`, `Std.string_join`,
  `Std.string_repeat`, `Std.string_pad_left/right`.
  **Not in Std:** `map`, `filter`, `reduce` (those are global builtins).
- **Mcp** — `Mcp.client_start(cmd, args)`, `Mcp.list_tools(c)`,
  `Mcp.call_tool(c, name, args)`, `Mcp.serve(%{tools: [...]})`
- **Vec** + **Embed** — vector store + OpenAI-compatible embeddings
- **Cron** — `Cron.every(ms, fn)`, `Cron.in_ms(ms, fn)` (one-shot,
  not `after` — that's reserved)
- **Prompt** — `Prompt.render(template, vars)` with `{{var}}` slots

# Minimal correct example (study the f-string and print)

Task: "print the sum of [1, 2, 3] as `sum=N`".

```sw
module Main

import Std

fun main() {
    xs = [1, 2, 3]
    total = Std.sum(xs)
    print(f"sum={total}")
}
```

Output:
```
sum=6
```

Note the `f` prefix on the format string. Without it the output would be
`sum={total}` (literal).

# Iteration without `[h | t]`

Recursive walk (when `Std.map`/`filter`/`reduce` don't fit):

```sw
fun sum_list(lst, acc) {
    if (length(lst) == 0) { acc }
    else { sum_list(tl(lst), acc + hd(lst)) }
}
```

Or with case (the only allowed list patterns are `[]`, fixed-length, `_`):

```sw
fun count(lst) {
    case lst {
        [] -> 0
        _  -> 1 + count(tl(lst))
    }
}
```

# General gotchas

- `++` concatenates strings AND lists (both work)
- `print(x)` adds a trailing newline; `print_inline(x)` doesn't
- Externally-called module functions are `Module.fn(args)`
- `main()` is the program entry point; runtime exits when it returns
- `case` arms separate by `;` or newline; catchall is `_ ->`
- No semicolon required at end of statement (but legal everywhere)
- Booleans are atoms `'true'` / `'false'`, not bare keywords

Now solve the task below. Output a single complete sw program in a
```sw block. Make it concise. Don't explain.
