# sw — the language reference

The `.sw` language is what swarmrt's compiler (`swc`) builds from. Erlang-shaped semantics (immutable data, message-passing processes, pattern-matched `receive`), C-flavoured syntax (curly braces). Compiles ahead-of-time to native binaries via C codegen.

This doc is the reference for someone writing sw code today. For the runtime's C API see [API_REFERENCE.md](API_REFERENCE.md). For the architecture overview see [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Table of contents

1. [Hello world](#1-hello-world)
2. [Modules and imports](#2-modules-and-imports)
3. [Functions](#3-functions)
4. [Values and types](#4-values-and-types)
5. [Operators](#5-operators)
6. [Control flow](#6-control-flow)
7. [Pattern matching](#7-pattern-matching)
8. [Processes and message passing](#8-processes-and-message-passing)
9. [ETS — shared mutable state](#9-ets--shared-mutable-state)
10. [Builtins reference](#10-builtins-reference)
11. [Known gotchas](#11-known-gotchas)
12. [Build pipeline](#12-build-pipeline)

---

## 1. Hello world

```sw
module Main

fun main() {
    print("hello, sw")
    sys_exit(0)
}
```

```bash
swc build hello.sw -o hello && ./hello
```

Top-level entry point is `main()` in the module named `Main`. `sys_exit(0)` is required to terminate cleanly — without it the runtime busy-waits in its scheduler loop.

---

## 2. Modules and imports

Each `.sw` file is one module. The first non-comment line MUST be `module <Name>`.

```sw
module Browser

import LLM
import Tools
import UI
```

Module names are CamelCase by convention. `swc` resolves imports by looking for `src/<Name>.sw` (case preserved) and falling back to `src/<name>.sw` (lowercase) — so `Main` lives at `src/main.sw` per legacy convention.

Imports are top-level only and resolved transitively at build time. There's no namespace per module — calling an imported function uses `ModuleName.func(...)` syntax:

```sw
result = LLM.chat(messages, opts)
```

`export` declarations document the public surface but aren't enforced — every function is callable as `Module.func` from any importer. List the names you mean to expose for documentation:

```sw
export [init, navigate, click, screenshot, close]
```

---

## 3. Functions

```sw
fun add(a, b) {
    a + b
}

fun greet(name) {
    "hello, " ++ name
}
```

- Function definitions use `fun name(params) { body }`.
- Body is one or more expressions; the last expression's value is the return value.
- Parameters are positional; default values: `fun greet(name = "world") { ... }`.
- No explicit `return` — Erlang-style trailing-expression-is-the-value.
- Recursion is the loop construct (no `for`/`while`). Tail calls are detected and optimised by the codegen, so unbounded tail recursion doesn't blow the stack.

```sw
fun count_down(n) {
    if (n <= 0) {
        "done"
    } else {
        print(to_string(n))
        count_down(n - 1)   # tail call — flat stack
    }
}
```

Multi-statement function bodies are fine; statements are separated by newlines (no `;` needed):

```sw
fun spawn_agent(name, role) {
    table = ets_get_or_create('agents')
    pid = spawn(agent_loop(name, role))
    ets_put(table, name, pid)
    pid
}
```

---

## 4. Values and types

| Type | Examples | Notes |
|---|---|---|
| **int** | `0`, `42`, `-7`, `1_000_000` | 64-bit signed |
| **float** | `0.2`, `3.14`, `-1.0e6` | 64-bit double |
| **atom** | `'ok'`, `'error'`, `'true'`, `'nil'` | symbolic constants, single-quoted |
| **string** | `"hello"`, `"line\nfeed"` | double-quoted, supports `\n \r \t \\ \" \e \0` |
| **list** | `[1, 2, 3]`, `["a", "b"]`, `[]` | linked list, `hd/tl/length` |
| **tuple** | `{1, "a", 'ok'}` | fixed-size, `elem(t, 0)` |
| **map** | `%{key: val}`, `%{name: "Sam", age: 30}` | atom-keyed by default |
| **pid** | (returned by `spawn`/`self`) | opaque process handle |
| **nil** | `nil` | the absence of value |

Booleans are atoms: `'true'` and `'false'`. Comparisons return atoms (`x == y` returns `'true'` or `'false'`).

Map literals use `%{...}` syntax — note the `%` is part of the opener:

```sw
person = %{name: "Sam", age: 30, role: "engineer"}
name = map_get(person, 'name')          # atom-key lookup
older = map_put(person, 'age', 31)      # functional update — original unchanged
```

Strings concat with `++` (auto-coerces non-strings):

```sw
greeting = "hello, " ++ name ++ "!"
log = "count: " ++ count            # int auto-coerced
```

For anything more than a couple of pieces, prefer **f-strings** or **`format`**:

```sw
greeting = f"hello, {name}!"                          # f-string interpolation
log      = f"req={req_id} status={code} ms={elapsed}" # any expression in {…}
templ    = format("hi {} ({} req(s))", name, count)   # positional placeholders
```

f-strings desugar to `to_string(expr)` + `++` chains under the hood, so they work with any value (composites render via `sw_val_format` — same shape as `print()`).

---

## 5. Operators

| Op | Meaning | Example |
|---|---|---|
| `+ - * /` | arithmetic (int or float) | `(a + b) * 2` |
| `%` | modulo (int or float) | `n % 8` |
| `++` | string OR list concat | `"a" ++ "b"`, `[1] ++ [2,3]` |
| `==` `!=` `<` `<=` `>` `>=` | comparison → `'true' / 'false'` | `n == nil` |
| `&&` `\|\|` | boolean (truthy semantics) | `(x != nil) && (y > 0)` |
| `\|` | list cons | `[h \| t]` (in patterns) |
| `\|>` | pipe — `x \|> f(y)` becomes `f(x, y)` | `lines \|> filter(non_blank)` |

Truthiness: `nil`, `'false'`, and `0` are falsy. Everything else is truthy.

There is no unary `not` — use `== 'false'` or `if/else` swap.

---

## 6. Control flow

### if / else

`if/else` is an EXPRESSION that returns a value:

```sw
status = if (count > 100) { "high" }
         else { if (count > 10) { "med" }
         else { "low" }}
```

Both branches must be present if the value is used. The body of each branch is one or more statements; the last is the value.

There is no `elif` keyword; chain via `else { if (...) { ... } else { ... } }`.

### receive (pattern-matched message receive)

```sw
receive {
    {'task', prompt, reply_to} ->
        result = do_work(prompt)
        send(reply_to, {'result', result})
    {'stop'} ->
        'ok'
    _ ->
        'ignore'
    after 5000 {
        on_idle()
    }
}
```

- Each arm is `pattern -> body`.
- `_` matches anything; specific tuples / atoms / values match exactly.
- Bound names (`prompt`, `reply_to`) capture parts of the message.
- `after MS { body }` fires if no message arrives within MS milliseconds.
- Selective receive: messages that don't match any arm STAY in the mailbox for the next `receive`.

Inside receive arm bodies, `;` DOES separate statements (it's a recognised statement separator within arm bodies).

---

## 7. Pattern matching

Patterns appear in `receive` arms, `case` expressions, and some other binding contexts. Supported:

| Pattern | Matches |
|---|---|
| `42`, `"foo"`, `'ok'`, `nil` | exact literal |
| `name` | anything, binds to `name` |
| `_` | anything, no binding |
| `{'tag', a, b}` | 3-tuple starting with atom `'tag'` |
| `[h \| t]` | non-empty list, head + tail |
| `[]` | empty list |
| `%{key: v}` | map containing `key`, binds value to `v` |

Within a single arm pattern, every named variable is bound for the body that follows.

### case — top-level pattern dispatch

```sw
fun classify(msg) {
    case msg {
        {'ok', v}      -> "ok: " ++ to_string(v)
        {'error', why} -> "err: " ++ to_string(why)
        n when n > 0   -> "positive: " ++ to_string(n)
        'done'         -> "done"
        _other         -> "unknown: " ++ to_string(_other)
    }
}
```

Same arm-clause shape as `receive` (pattern, optional `when guard`, body). Runs against an arbitrary value, not the mailbox. Falls through to the next clause if a guard rejects. Returns the body of the first matching arm, or `nil` if nothing matches.

This is the structured replacement for nested `if/else` ladders. Use it whenever you'd write `if (x == 'a') { ... } else { if (x == 'b') { ... } else { ... } }`.

---

## 8. Processes and message passing

The runtime is built around lightweight actors. Each process has its own heap and mailbox. Communication is by message passing, not shared memory.

### Spawning

```sw
pid = spawn(my_loop(arg1, arg2))   # `my_loop(...)` is the function call shape;
                                    # spawn intercepts and runs in a new process
```

`spawn(fn(args))` is the idiom — pass the function call shape, and `spawn` evaluates it in a new process. The outer process gets back the new PID immediately.

### Identity

```sw
my_pid = self()                     # this process's pid
```

### Registry

Atom-keyed global name registry — useful for "well-known" processes like a main loop.

```sw
register('main_agent', self())      # in the main process at startup
main_pid = whereis('main_agent')    # from anywhere, look up by name
if (main_pid != nil) {
    send(main_pid, {'tick', 42})
}
```

### Sending

```sw
send(pid, message)                  # message is any sw value
```

Sends are non-blocking and unordered between distinct sender→receiver pairs. Within a single sender→receiver pair, message order is preserved.

### Receiving

See [§ 6](#6-control-flow) above.

### Common pattern: GenServer-style loop

```sw
fun start() {
    spawn(loop(initial_state()))
}

fun loop(state) {
    receive {
        {'get', key, reply_to} ->
            send(reply_to, map_get(state, key))
            loop(state)
        {'put', key, value} ->
            loop(map_put(state, key, value))
        {'stop'} ->
            'ok'
    }
    # If no after clause, control comes here only if receive returns
    # without recursing (e.g. via the 'stop' arm). Otherwise loop
    # tail-recurses inside the matched arm.
}
```

---

## 9. ETS — shared mutable state

Maps in sw are immutable (every `map_put` returns a new map). When you actually need shared mutable state across processes — counters, registries, caches — use ETS.

```sw
table = ets_new()                    # returns table id
ets_put(table, 'counter', 0)         # set
v = ets_get(table, 'counter')        # read
ets_put(table, 'counter', v + 1)     # update
ets_delete(table, 'counter')         # remove
entries = ets_list(table)            # all {key, val} as a list
n = ets_count(table)                 # number of entries
```

ETS tables are global — any process holding the table id can read/write. They're the standard pattern for things like swarm-code's `swarm_registry`, `todos_table`, `browser_table`.

Keys can be atoms, strings, or ints. Values can be any sw value (including maps and lists).

---

## 10. Builtins reference

Every function callable directly without `Module.` prefix. Grouped by category.

### IO & system
| | |
|---|---|
| `print(...)` | stdout, trailing newline |
| `print_inline(...)` | stdout, no newline |
| `read_line(prompt?)` | stdin, returns string or `nil` on EOF |
| `read_char()` | single keypress (raw mode) |
| `read_choice(header, options)` | arrow-key picker → int index, -1 on cancel |
| `getenv(name)` | env var or `nil` |
| `sys_exit(code?)` | terminate process (0 if omitted) |
| `timestamp()` | ms since epoch |
| `sleep(ms)` | block this process for ms |
| `term_cols()` | terminal width via TIOCGWINSZ |
| `shell(cmd)` | run shell command → `{exit_code, stdout_string}` |

### Strings
| | |
|---|---|
| `string_length(s)` | byte length |
| `string_sub(s, start, len)` | substring |
| `string_split(s, sep)` | list of strings |
| `string_replace(s, old, new)` | replace ALL |
| `string_contains(s, sub)` | `'true'/'false'` |
| `string_starts_with(s, prefix)` | `'true'/'false'` |
| `string_ends_with(s, suffix)` | `'true'/'false'` |
| `string_index_of(s, sub)` | int (-1 if not found) |
| `string_trim(s)` | strip whitespace |
| `string_upper(s)` / `string_lower(s)` | case |
| `string_truncate(s, max_len)` | truncate to max_len |
| `to_string(v)` | any → string (tuples / lists / maps render via the formatter) |
| `format(template, args...)` | `format("hi {} (#{})", name, n)` — `{}` placeholders, `{{` / `}}` escape |
| `strip_html(html)` | tags stripped, entities decoded |
| `clean_json(s)` | strip code fences and trailing commas |

### Base64
| | |
|---|---|
| `base64_encode(s)` | string → base64 string |
| `base64_decode(s)` | base64 string → string (or `nil` on bad input) |

### Lists & maps
| | |
|---|---|
| `length(lst)` | item count |
| `hd(lst)` | first element |
| `tl(lst)` | rest |
| `elem(tuple, i)` | nth element |
| `list_append(lst, x)` | new list with x appended |
| `map(lst, fn)` | apply fn to each, return new list |
| `filter(lst, pred)` | keep where pred → truthy |
| `reduce(lst, init, fn)` | foldl |
| `pmap(lst, fn)` | parallel map (each fn call in own process) |
| `map_get(m, k)` | value or `nil` |
| `map_put(m, k, v)` | new map (functional update) |
| `map_remove(m, k)` | new map without `k` (returns `m` if absent) |
| `map_keys(m)` / `map_values(m)` | list of keys / values |
| `map_merge(m1, m2)` | new map; `m2`'s keys win on collision |
| `map_has_key(m, k)` | `'true'` / `'false'` |
| `map_size(m)` | int |

### ETS
| | |
|---|---|
| `ets_new()` | new table id |
| `ets_get(t, k)` / `ets_put(t, k, v)` / `ets_delete(t, k)` | basic ops |
| `ets_list(t)` | list of `{k, v}` tuples |
| `ets_count(t)` | size |

### Files
| | |
|---|---|
| `file_read(path)` | full contents as string, or `nil` |
| `file_write(path, content)` | `'ok'/'error'` |
| `file_exists(path)` | `'true'/'false'` |
| `file_delete(path)` | `'ok'/'error'` |
| `file_list(dir)` | list of names |

### JSON
| | |
|---|---|
| `json_encode(v)` | sw value → JSON string. Auto-grows; no 256KB cap |
| `json_decode(s)` | JSON string → sw value (`\uXXXX` escapes decoded to UTF-8) |

### HTTP & WebSocket
| | |
|---|---|
| `http_get(url, headers?)` | GET → response body string. Auto-grows |
| `http_post(url, headers, body)` | POST → string. **ESC-interruptible**; returns `"__INTERRUPTED__"` on Ctrl-C |
| `http_post_stream(url, headers, body, opts?)` | streams to stdout incrementally; reasoning channel; ESC interrupt |
| **WS server** (LiveView): `http_listen`, `http_respond`, `ws_send`, `ws_close`, `ws_set_handler`, `live_js` |
| **WS client** (CDP / external): `wsc_connect(ws_url)` → handle, `wsc_send(h, text)`, `wsc_recv(h, timeout_ms)` → string, `wsc_close(h)` |

### Browser
| | |
|---|---|
| `chrome_launch(port?, headless?)` | spawn Chrome with `--remote-debugging-port`, return port. Discovers Chrome / Chromium / Brave / Edge / Arc / Playwright cache |

### Process / runtime
| | |
|---|---|
| `spawn(fn(args))` | new process, returns pid |
| `self()` | own pid |
| `send(pid, msg)` | non-blocking |
| `register(name, pid)` / `whereis(name)` | atom registry |
| `process_info(pid)` | inspection map |
| `process_list()` | all live pids |
| `registered()` | all registered atoms |

### Distribution (multi-node)
| | |
|---|---|
| `node_start(name, port)` / `node_stop()` | become a node |
| `node_connect(name, host, port)` / `node_disconnect(name)` | mesh |
| `node_send(node, regname, msg)` | cross-node send |

### LLM (built-in)
| | |
|---|---|
| `llm_complete(prompt, opts)` | one-shot completion via Otonomy proxy |
| `llm_stream(prompt, opts)` | streaming variant |

### Async helpers (OTP-style)
| | |
|---|---|
| `every(ms, fn)` / `after(ms, fn)` | timer-driven |
| `pubsub_subscribe(topic)` / `pubsub_broadcast(topic, msg)` / `pubsub_unsubscribe(topic)` | pub/sub |
| `breaker_new` / `breaker_call` / `breaker_state` | circuit breaker |

### PDF
| | |
|---|---|
| `pdf_text(path)` | extract text |
| `pdf_pages(path)` | int page count |
| `pdf_meta(path)` | metadata map |

---

## 11. Known gotchas

These are real, hit-during-development quirks. Skim before you write a lot of sw.

- **`;` and newline both separate statements** — in any block: function body, if-branch, else-branch, receive arm. Both of these parse and run identically:
  ```sw
  if (x) { a = compute() ; print(a) }
  if (x) {
      a = compute()
      print(a)
  }
  ```
  *(Was a gotcha until 2026-05-15 — `;` in if-branches used to error.)*

- **C-reserved words ARE legal sw identifiers.** Since 2026-05-15 the codegen mangles `inline`, `static`, `extern`, `const`, `register`, `volatile`, `auto`, `goto`, `restrict`, `signed`, `unsigned`, `union`, `enum`, `struct`, `typedef`, `return`, `break`, `continue`, etc. by appending `_sw` at every C-emission site. Source-level lookup is unchanged, so the mangling is invisible to your sw code. Use them freely.

- **Variable scope shadowing FIXED 2026-05-15.** Earlier versions silently emitted "use of undeclared identifier" when the same name was used in two `if/else` branches. Codegen now snapshots and restores the declaration list around each branch. If you see this on an older swarmrt, rebuild swc.

- **`#line` directives surface sw line numbers in C errors.** When a generated-C compile fails, the error points at `src/<Module>.sw:<line>` instead of `/tmp/swc_*.c`. **Per-statement accuracy** since the late-2026-05-15 codegen pass — points at the exact failing line, not the function start.

- **No userland `exit/2`** for killing other processes. Send a `{'stop'}` message and let the receiver clean up on its next `receive`. True preemption needs runtime support not yet exposed.

- **The runtime exits when `main()` returns.** Go-style, not Erlang-style — when your program's `main` function finishes, the runtime tears down the schedulers and the process exits with code 0. If you want a long-running server, end `main` with a permanent `receive { _other -> ... }` arm or `sleep(N)` loop. (Until 2026-05-15 the generated `main()` ran an infinite `usleep` loop, so every program hung after main finished. That's now fixed.)

- **Map literal `%{` is a SINGLE TOKEN.** `expr % {…}` does NOT parse as "modulo of expr by a brace block" — `%` and `{` need a space if you want modulo of `expr` by some expression starting with `{` (which is uncommon anyway). Use `expr % some_var` for clarity.

---

## 12. Build pipeline

```bash
# Build the compiler + library (do this once, or whenever you touch
# anything in swarmrt/src/)
cd /path/to/swarmrt
make swc libswarmrt

# Compile a sw file → standalone native binary
bin/swc build my_program.sw -o my_program

# Multi-module: imports auto-resolved from the same directory
bin/swc build src/main.sw -o bin/program

# Optimised + obfuscated build
bin/swc build src/main.sw -o bin/program -O --obfusc --strip
```

The compiler emits C to `/tmp/swc_<Module>_*.c`, then invokes `cc` linking against `libswarmrt.a`. Errors at the C step now include `#line` directives so they point back at sw source. If swc itself errors at the parse stage, the message includes the sw line directly.

Don't `rm -rf bin/` in the swarmrt dir — that wipes `bin/swc` and `bin/libswarmrt.a`. Use `make swc libswarmrt` to rebuild after a clean.
