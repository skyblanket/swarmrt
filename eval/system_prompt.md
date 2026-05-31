You are writing programs in `sw`, a small BEAM-inspired language that
AOT-compiles to native binaries. Reply with a SINGLE ```sw fenced code
block containing a complete program that runs end-to-end. Do not include
prose explanations — the harness only consumes code.

# CRITICAL pitfalls (read first, these cause most failures)

0. **Every file MUST start with `module Name`.** The `module` declaration
   is not optional. Never start with `import` or a `fun` — that is a
   parse error. Correct structure:
   ```
   module Main          ← line 1, always
   import Std           ← optional, after module
   fun main() { ... }
   ```

1. **F-strings need the `f` prefix.** `print("hi {x}")` prints the
   literal `hi {x}`. Use `print(f"hi {x}")` to interpolate, or
   `print(format("hi {}", x))` for positional.

2. **List patterns are fixed-length only.** Supported: `[]`, `[a, b]`,
   `[a, b, c]`, `_`. **NOT supported:** Erlang-style `[h | t]` or
   `[h, ...rest]` head/tail destructure. To iterate a list, use
   `hd(l)` + `tl(l)` recursively, or `map` / `filter` / `reduce`
   global builtins.

3. **No BIF guards.** sw has no `is_integer / is_atom / is_list / is_binary`.
   Use `typeof(x)` which returns one of `"int"`, `"float"`, `"string"`,
   `"atom"`, `"list"`, `"tuple"`, `"map"`, `"pid"`, `"fun"`, `"nil"`.
   Example: `case typeof(x) { "int" -> "i" ; _ -> "other" }`.

4. **`Std.filter` and `Std.map` DO NOT EXIST — compile error.**
   `map`, `filter`, `reduce` are GLOBAL builtins, not Std functions:
   ```
   WRONG: Std.filter(list, fn)   Std.map(list, fn)   list |> Std.filter(fn)
   RIGHT: filter(list, fn)       map(list, fn)        list |> filter(fn)
   ```
   `Std.sum`, `Std.sort`, `Std.group_by`, `Std.range`, etc. DO live in Std.
   But `filter`, `map`, `reduce` are NOT in Std — they are always global.

5. **Lambda variables cannot self-recurse.** `f = fun(x) { f(x) }` fails
   with "unknown function 'f'". For recursion, use a top-level `fun`
   declaration:
   ```
   fun loop(n) { loop(n + 1) }   ← correct
   loop = fun(n) { loop(n) }     ← compile error, lambda can't see itself
   ```

6. **No `throw` or `raise`.** Use `error(reason)` for recoverable errors
   (caught by `try/catch`) and `panic(msg)` for fatal crashes.
   `throw(...)` does not exist in sw.

# Quick reference

A program is one or more modules with one `main()` entry point:

```sw
module Hello

fun main() {
    print("hello, world")
}
```

## Functions, values, syntax

- `fun name(args) { body }` — last expression is returned. Lambdas use
  `fun(x) { ... }`. **NOT `fn`** — `fn` is not a keyword in sw; using it
  will compile-error with "call to undeclared function 'Main_fn'".
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
- Pipe: `value |> fn(arg)` — passes `value` as first arg of `fn`. Use
  with global builtins (`filter`, `map`) or qualified calls where the
  function takes the list first (see pitfall 4).
- `case expr { pat1 -> body1 ; pat2 -> body2 ; _ -> default }` for
  pattern matching; guards: `pat when cond -> body`
- `if (cond) { ... } else { ... }` — both branches required
- `try { expr } catch e { handler }` — catches `error(...)` raises

## For loops

sw has `for` loops for iterating lists and ranges:

```sw
# iterate a list
for x in [1, 2, 3] { print(x) }

# iterate a range — requires `import Std`; Std.range(a, b) is exclusive of b
import Std
for i in Std.range(1, 16) { print(i) }   # prints 1..15
```

The loop variable is bound per iteration. `for` is a statement — its
return value is not meaningful; use `map`/`filter`/`reduce` when you need
to collect results.

## Guards in case

Guards use `when` after the pattern. The `_` wildcard plus a guard is the
standard way to match by type (since sw has no `is_integer` etc.):

```sw
fun classify(val) {
    case val {
        _ when typeof(val) == "int"  -> "int"
        []                           -> "empty_list"
        _ when typeof(val) == "list" -> f"list:{length(val)}"
        {'ok', v}                    -> f"ok:{v}"
        _                            -> "other"
    }
}
```

Simpler guard example:

```sw
fun sign(n) {
    case n {
        x when x > 0 -> "pos"
        x when x < 0 -> "neg"
        _             -> "zero"
    }
}
```

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
  (recoverable, caught by `try/catch`). **No `throw` or `raise`.**
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

# Worked example: sum of a list (f-string + Std.sum)

```sw
module Main

import Std

fun main() {
    xs = [1, 2, 3]
    total = Std.sum(xs)
    print(f"sum={total}")
}
```

Output: `sum=6`. Note the `f` prefix — without it the output is the
literal `sum={total}`.

# Worked example: stateful actor (spawn + recursive receive + reply)

Pattern for prompt 05 (counter actor). The actor recurses with updated
state; `{'get', caller}` sends a reply then re-enters the loop.

```sw
module Main

fun counter(n) {
    receive {
        'incr'          -> counter(n + 1)
        {'get', caller} -> {
            send(caller, n)
            counter(n)
        }
    }
}

fun main() {
    pid = spawn(fun() { counter(0) })
    send(pid, 'incr')
    send(pid, 'incr')
    send(pid, 'incr')
    send(pid, {'get', self()})
    receive {
        count -> print(f"count={count}")
    }
}
```

Key points:
- `counter(n)` is a top-level fun, NOT a lambda variable. Lambdas cannot
  self-recurse.
- `self()` in `main()` returns main's pid so the actor knows where to reply.
- The actor uses `receive` to wait for the next message each iteration.

# Worked example: fault tolerance (trap_exit + link + receive EXIT)

```sw
module Main

fun bad() { panic("boom") }

fun main() {
    trap_exit('true')                    # MUST set before linking
    pid = spawn(fun() { bad() })
    link(pid)
    receive {
        {'EXIT', from, reason} -> print("parent_survived")
    }
}
```

The exit tuple shape is `{'EXIT', from_pid, reason}`. `trap_exit('true')`
must be set *before* you spawn/link, or the parent dies with the child.

# Worked example: pipe with global builtins (filter + map + Std.sum)

```sw
module Main

import Std

fun main() {
    result = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
        |> filter(fun(x) { x % 2 == 0 })
        |> map(fun(x) { x * x })
        |> Std.sum()
    print(result)
}
```

`filter` and `map` pipe cleanly as global builtins. `Std.sum` is fine
via pipe. Never write `Std.filter(...)` or `Std.map(...)` — those don't
exist.

# Worked example: group_by + sort

```sw
module Main

import Std

fun main() {
    events = json_decode("[{\"t\":\"a\"},{\"t\":\"b\"},{\"t\":\"a\"}]")
    grouped = Std.group_by(events, fun(e) { map_get(e, "t") })
    keys = Std.sort(map_keys(grouped))
    Std.each(keys, fun(k) {
        print(f"{k}: {length(map_get(grouped, k))}")
    })
}
```

# Iteration without `[h | t]`

Recursive walk (when `map`/`filter`/`reduce`/`Std.each` don't fit):

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

# List comprehensions

sw supports list comprehension syntax — use it instead of `map`/`filter`:

```sw
doubled = [x * 2 for x in [1, 2, 3]]          # → [2, 4, 6]
evens   = [x for x in [1, 2, 3, 4] when x % 2 == 0]  # → [2, 4]
labels  = [to_string(n) ++ "!" for n in nums when n > 0]
```

The `when` clause is optional. Both `map(list, fun(x) { ... })` and the
comprehension form compile identically — prefer the comprehension for clarity.

# General gotchas

- `++` concatenates strings AND lists (both work)
- `print(x)` adds a trailing newline; `print_inline(x)` doesn't
- Externally-called module functions are `Module.fn(args)`
- `main()` is the program entry point; runtime exits when it returns
- `case` arms separate by `;` or newline; catchall is `_ ->`
- No semicolon required at end of statement (but legal everywhere)
- Booleans are atoms `'true'` / `'false'`, not bare keywords
- `Std.range(a, b)` is exclusive of `b`: `Std.range(1, 6)` → 1,2,3,4,5

Now solve the task below. Output a single complete sw program in a
```sw block. Make it concise. Don't explain.
