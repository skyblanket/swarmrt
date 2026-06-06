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
11. [Errors and panics](#11-errors-and-panics)
12. [Known gotchas](#12-known-gotchas)
13. [Build pipeline](#13-build-pipeline)

---

## 1. Hello world

```sw
module Main

fun main() {
    print("hello, sw")   # => hello, sw
}
```

```bash
swc build hello.sw -o hello && ./hello
```

Top-level entry point is `main()` in the module named `Main`. The program exits cleanly when `main()` returns — no `sys_exit` needed. The codegen wraps `main()` in a `_main_entry` thunk that signals a condition variable on return; the generated C `main` waits on that signal, calls `sw_shutdown(0)`, and returns 0. Use `sys_exit(code)` only when you want to exit with a non-zero status code (e.g. to signal an error to the calling shell).

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

### Module-level `let` globals

A module may declare up to 16 named constants at the top level using `let`. These are initialized once at compile time and visible to every function in the module.

```sw
module Config

let max_retries = 5
let base_url = "https://api.example.com"
let default_timeout = 30000
let service_name = 'my_service'
```

Supported literal types for module globals: `int`, `float`, `string`, and `atom`. Complex expressions, function calls, and list/map literals are **not** supported at module scope — put those inside `fun init()` or a similar startup function.

Globals are read-only after initialization. Any assignment to a module-global name inside a function creates a local variable that shadows the global for that scope; the global itself is unchanged.

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

### Anonymous functions (lambdas)

An anonymous function is `fun(params) { body }` — or the shorter `fn(params) { body }` (the two are interchangeable). A lambda is a **first-class value** and a primary expression, so it is valid **anywhere an expression is** — as a call argument, a list/map element, a return value, or an assignment RHS:

```sw
dbl  = fn(x) { x * 2 }            # assignment RHS
doubled = map(fn(x) { x * 2 }, [1, 2, 3])    # call argument -> [2, 4, 6]
ops  = [fn(){ 1 }, fn(){ 2 }]     # list element
m    = %{ inc: fn(x) { x + 1 } }  # map value
fun adder(n) { fn(x) { x + n } }  # returned from a function
Cron.every(200, fn() { check_inbox() })     # the common scheduler idiom
```

`fn` is **not** a reserved word — it is only a lambda when written in the `fn(...) { ... }` shape, so it stays a perfectly good ordinary variable / parameter name (the stdlib uses `fn` as a callback parameter throughout).

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

**Records** are just tagged maps — named fields instead of fragile tuple positions. There is no `record` keyword and no new type: a record is a `%{}` with a `'__record'` tag, built and checked by the `Record` library (`import Record`). Field access is the ordinary `.field` you already have (it desugars to `map_get`):

```sw
import Record

c = Record.build('Call', ['id', 'from', 'state'],
                 %{id: 7, from: "+1555", state: 'ringing'})

c.state                          # 'ringing'  — named, not elem(c, 2)
Record.is(c, 'Call')             # 'true'
c2 = Record.update(c, 'state', 'connected')   # functional update of a checked field
```

`Record.build` panics on a missing field (fail-fast); `Record.new` returns `{'ok', rec}` / `{'error', reason}` for recoverable validation (same split as [§11 Errors and panics](#11-errors-and-panics)). Records pattern-match like any map (`case c { %{__record: 'Call', state: s} -> … }`), send over messages, and `json_encode` cleanly. It is **not** a type system: fields aren't typed and nothing is rejected at compile time — the only check is field-presence at construction. Use a record when a tuple's positions have started to feel arbitrary.

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
| `\|` | list cons | `[h \| t]`, `[a, b \| rest]` (patterns + construction) |
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

An arm body may be a bare tuple literal — `_ -> {'error', why}` returns the tuple `{'error', why}` directly, no parentheses needed. A `{` after `->` is read as a tuple when it carries a top-level comma (`{a, b}`) and as a statement block when it does not (`{ x = ...; x }`); both forms work, in `receive` arms too:

```sw
case parse(line) {
    {'ok', v}    -> {'parsed', v}            # bare tuple result
    {'error', e} -> { log(e) ; {'error', e} } # block: a statement, then a tuple
    _            -> {'error', 'unknown'}
}
```

This is the structured replacement for nested `if/else` ladders. Use it whenever you'd write `if (x == 'a') { ... } else { if (x == 'b') { ... } else { ... } }`.

### with — happy-path error chains

A fallible pipeline — decode, check, extract, validate, call — is a chain where each step must succeed before the next runs, and *any* failure short-circuits to one place. Nesting `case` for that is verbose and easy to get wrong. `with` is the construct for it (Elixir semantics):

```sw
with p1 <- e1, p2 <- e2, ... { body } else { other -> handler }
```

Evaluate `e1` and match it against `p1`; on a match, continue to the next bind; on a *mismatch*, bind the non-matching value to `other` and run the `else` arm. If every bind matches, run `body`. The bound names from each pattern are visible to the later binds and to `body`.

The idiom is for each step to return a tagged result — `{'ok', v}` on success, anything else on failure — and to bind `{'ok', v}`. The first step that isn't `{'ok', _}` lands in the `else` arm, value intact:

```sw
module WithDemo
export [main]

fun step(label, v) {
    if (v < 0) { {'error', label} }
    else { {'ok', v * 2} }
}

fun pipeline(a, b) {
    with {'ok', x} <- step("a", a),
         {'ok', y} <- step("b", b) {
        {'ok', x + y}
    } else {
        {'error', why} -> f"failed at {why}"
    }
}

fun main() {
    print(pipeline(2, 5))        # => {:ok, 14}
    print(pipeline(2, 0 - 3))    # => failed at b
    print(pipeline(0 - 1, 5))    # => failed at a
}
```

`with` desugars at parse time into nested `case`, so it has the same pattern-matching power (tuples, maps, lists, literals) and the exact same behavior in `swc run` and a compiled binary. There are no per-bind `when` guards — match a pattern instead. A single bind (`with p <- e { ... } else { o -> ... }`) is fine; it's just a one-arm chain.

---

## 8. Processes and message passing

The runtime is built around lightweight actors. Each process has its own heap and mailbox. Communication is by message passing, not shared memory.

### Spawning

```sw
pid = spawn(my_loop(arg1, arg2))   # `my_loop(...)` is the function call shape;
                                    # spawn intercepts and runs in a new process
```

`spawn(my_loop(args))` is the idiom — pass the function call shape, and `spawn` evaluates it in a new process. The outer process gets back the new PID immediately.

`spawn` also accepts a **function value**: an inline lambda, a closure-valued local, or a function by-name. All run with zero arguments in the new process (capture state in the closure):

```sw
spawn(fn() { worker_loop(0) })   # inline lambda
job = fn() { do_work() }
spawn(job)                       # a closure-valued local
spawn(my_loop)                   # a function by name (runs my_loop())
```

`spawn_monitor(...)` takes the same shapes and additionally returns `{pid, ref}` so the parent gets a `{'DOWN', ref, ...}` message when the child exits.

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
| `exec_argv(cmd, args)` → `{code, out}` | fork+exec with no shell — safe for user data |

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
| `format(template, args...)` | `format("hi {} ({} req)", name, n)` — positional `{}` placeholders, `{{` / `}}` escape to literal braces. (For inline-expression interpolation use an f-string: `f"hi {name} #{n}"`.) |
| `strip_html(html)` | tags stripped, entities decoded |
| `clean_json(s)` | strip code fences and trailing commas |

### Base64
| | |
|---|---|
| `base64_encode(s)` | string → base64 string |
| `base64_decode(s)` | base64 string → string (or `nil` on bad input) |

### Bytes
A length-carrying, NUL-safe byte vector (`typeof` → `"bytes"`). Unlike a
string, it survives embedded `0x00` bytes, so it is the right type for raw
binary (PCM audio, protocol frames). Values come from builtins, never a
source literal. Equality is by content (`memcmp`); bytes are equal only to
bytes. `length(b)` and `print`/`to_string` (renders `<<d,d,...>>`) work on
bytes too. Bytes copy correctly over `send` and can be used as ETS keys.

| | |
|---|---|
| `bytes_from_base64(s)` | base64 string → bytes (NUL-safe; `nil` on bad input) |
| `bytes_to_base64(b)` | bytes → base64 string |
| `bytes_from_ints(list)` | list of ints `0..255` → bytes (renders `<<72,73>>`) |
| `byte_size(b)` | length in bytes (`0` for non-bytes) |
| `byte_at(b, i)` | byte `0..255` at index `i` (panics out of range) |
| `byte_slice(b, start, len)` | subrange; `len` clamps to end |
| `bytes_concat(a, b)` | new bytes `a ++ b` |
| `string_to_bytes(s)` | string chars → bytes |
| `bytes_to_string(b)` | bytes → string (truncates at first NUL by design) |
| `audio_ulaw_to_pcm16_b(b)` | mu-law bytes → PCM16 bytes (codec twin) |
| `audio_pcm16_to_ulaw_b(b)` | PCM16 bytes → mu-law bytes (codec twin) |
| `audio_resample_b(b, from, to)` | PCM16 bytes resample (codec twin) |

### Crypto
| | |
|---|---|
| `ed25519_verify(public_key, signature, message)` | verify an Ed25519 signature → `'true'` / `'false'` (`nil` if this build has no TLS) |

`ed25519_verify` uses OpenSSL's EVP one-shot verify and is gated on the same
TLS build as `wsc_connect_tls` (auto-on for Linux; macOS opt-in via
`make SWARMRT_TLS=1`). Without TLS it returns `nil` with a one-line stderr note
(the caller should fail closed, e.g. reject the webhook). Inputs are flexible:
`public_key` and `signature` may be raw `bytes` (32 / 64 bytes) **or** base64
strings (decoded for you — the shape Telnyx delivers a webhook key + signature);
`message` is the signed bytes, either `bytes` or a string taken verbatim (NOT
base64). Wrong-length / undecodable input returns `'false'` (never crashes).

### Lists & maps
| | |
|---|---|
| `length(lst)` | item count |
| `hd(lst)` | first element |
| `tl(lst)` | rest |
| `elem(tuple, i)` | nth element |
| `list_append(lst, x)` | new list with x appended |
| `map(fn, lst)` | apply fn to each, return new list (either arg order accepted) |
| `filter(lst, pred)` | keep where pred → truthy |
| `reduce(fn, lst, init)` | foldl |
| `pmap(fn, lst)` | parallel map (each fn call in own process); either arg order accepted, like `map`. **Fires ALL items at once (no concurrency cap) and silently maps a slow item to `nil` on a fixed ~5s wall.** For rate-limited fan-out — "run 100 LLM calls, 5 at a time", with tagged per-item results — use `Std.task_stream` (in `lib/Std.sw`) instead. |
| `map_new()` | new empty map (same as the `%{}` literal) |
| `map_get(m, k)` | value or `nil` |
| `map_get(m, k, default)` | 3-arg form: value, or `default` if `k` is absent |
| `map_put(m, k, v)` | new map (functional update) |
| `map_remove(m, k)` | new map without `k` (returns `m` if absent) |
| `map_keys(m)` / `map_values(m)` | list of keys / values |
| `map_merge(m1, m2)` | new map; `m2`'s keys win on collision |
| `map_has_key(m, k)` | `'true'` / `'false'` |
| `map_size(m)` | int |

### Numbers & type checks
| | |
|---|---|
| `abs(n)` | absolute value (int or float) |
| `to_int(v)` | parse a string (`to_int("42")` → `42`), truncate a float, or pass an int; **`nil`** on a non-numeric string |
| `to_float(v)` | parse a string (`to_float("3.14")` → `3.14`) or widen a number; **`nil`** on a non-numeric string |
| `uuid()` | a random RFC-4122 v4 UUID string (e.g. `"f47ac10b-58cc-4372-a567-0e02b2c3d479"`) |
| `now_iso()` | current UTC time as ISO-8601 (`"2026-06-06T13:08:05Z"`) |
| `ord(s)` | first byte of a string as an int (`ord("A")` → `65`) |
| `typeof(v)` | type as a string: `"int"`, `"float"`, `"string"`, `"atom"`, `"list"`, `"map"`, `"tuple"`, `"bytes"`, `"pid"`, … |
| `is_list(v)` | `'true'` / `'false'` |
| `is_map(v)` | `'true'` / `'false'` |

### Math (`import Math`)
Thin wrappers over the libm-backed builtins plus a few pure-sw helpers. All trig in radians. `sqrt`/`sin`/`cos`/`pow`/`exp`/`log` return floats; `floor`/`ceil`/`round` return ints. Each accepts an int OR a float.

| | |
|---|---|
| `Math.sqrt(x)` `Math.sin(x)` `Math.cos(x)` | libm-backed |
| `Math.pow(b, e)` `Math.exp(x)` `Math.log(x)` | libm-backed |
| `Math.floor(x)` `Math.ceil(x)` `Math.round(x)` | → int |
| `Math.float(x)` | alias for the `to_float` builtin |
| `Math.pi()` | `3.141592653589793` |
| `Math.min(a, b)` `Math.max(a, b)` `Math.clamp(x, lo, hi)` | pure-sw helpers |

### ETS
| | |
|---|---|
| `ets_new()` | new table id |
| `ets_get(t, k)` / `ets_put(t, k, v)` / `ets_delete(t, k)` | basic ops |
| `ets_list(t)` | list of `{k, v}` tuples |
| `ets_count(t)` | size |
| `ets_update_counter(t, k, delta, initial)` | atomic `+= delta`, seeds `initial+delta` if missing; returns new int |
| `ets_cas(t, k, expected, new)` | compare-and-swap; `'true'` if swapped, `'false'` if mismatch / missing |
| `ets_take(t, k)` | atomic get-and-delete; returns value or `nil` |
| `ets_update(t, k, fun)` | reserved — passing `.sw` lambdas to a builtin needs a runtime helper; currently returns `nil`. Use cas/get-put loops for now. |

### Files
| | |
|---|---|
| `file_read(path)` | full contents as string, or `nil` (current 1 MB cap) |
| `file_read_bytes(path)` | full contents as **bytes** (NUL-safe — for binary files), or `nil` |
| `file_write(path, content)` | `'ok'/'error'` — not crash-safe; use `file_atomic_write` for state files |
| `file_write_bytes(path, b)` | write a bytes value verbatim; `'ok'/'error'` |
| `file_atomic_write(path, content)` | writes `path.tmp.<pid>` then `rename(2)` — survives a crash mid-write |
| `file_rename(src, dst)` | `'ok'/'error'` — wraps `rename(2)` |
| `file_stat(path)` | `%{size, mtime, mode, is_dir, exists}` map, or `nil` if missing |
| `file_temp(prefix)` | unique tmp file path via `mkstemp` (`<prefix>XXXXXX`) |
| `file_exists(path)` | `'true'/'false'` |
| `file_delete(path)` | `'ok'/'error'` |
| `file_mkdir(path)` | `'ok'/'error'` |
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
| `http_request(url, opts)` | **status-aware** request → `%{status: int, body: string, headers: %{lowercased keys}}` on a completed transport (incl. 4xx/5xx — the status is surfaced, not hidden), or `{'error, reason}` if the request never completed. `opts` is a map: `method` (default `"GET"`), `headers` (a `%{name=>value}` MAP **or** a list of `{name, value}` tuples), `body` (string). Unlike `http_post`/`http_get` (body-or-nil, status hidden) it lets a caller tell a 200 from a 4xx/5xx with a body. `http_post`/`http_get` are unchanged |
| `http_post_stream(url, headers, body, [pid, name])` | streams to stdout incrementally; reasoning channel; ESC interrupt. Returns a **tagged** result: `{'ok, openai_json}` on success, `{'error, reason}` on curl-failure / non-2xx / empty-or-unparseable stream. Parses both `data: {...}` and `data:{...}` SSE framing |
| **HTTP/WS server**: `http_listen`, `http_respond`, `ws_send`, `ws_close`, `ws_set_handler`, `ws_request_headers`, `ws_request_path`, `live_js` |
| `http_listen(port)` delivers `{'http_request', conn, method, path, headers, body}` (HTTP) and `{'ws_connect', conn, path}` / `{'ws_message', conn, text}` / `{'ws_close', conn}` (WS) to the handler. `headers` is a **MAP with lowercased keys** (bearer/signature reads). For a WS connection, `ws_request_headers(conn)` → the UPGRADE request's header MAP and `ws_request_path(conn)` → its path |
| **WS client** (CDP / external): `wsc_connect(ws_url)` → handle, `wsc_connect_tls(wss_url)` → handle (TLS `wss://`), `wsc_send(h, text)`, `wsc_recv(h, timeout_ms)` → string, `wsc_set_handler(h, pid)` (deliver frames to a process as messages), `wsc_close(h)` |

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
| `process_info(pid)` | inspection map: `pid, status, reductions, messages, heap_used, heap_size`, plus `name` / `parent` (parent's pid) when set |
| `process_list()` | all live pids |
| `registered()` | all registered `{name, pid}` pairs |
| `import Swarm` → `Swarm.top()` / `Swarm.tree()` | live snapshot (list of process maps) / supervision forest, grouped by `parent`; `print_top()` / `print_tree()` format them. Compiled-only (see note below). |
| `pid_alive(os_pid)` | `'true'` if the OS process (integer pid from `subprocess_spawn`) is still running, `'false'` if it has exited. Uses `kill(pid, 0)` — no signal sent. Does **not** take sw actor pids returned by `spawn()`. |
| `link(pid)` / `unlink(pid)` | bidirectional link — when either side dies abnormally the other gets an exit signal |
| `monitor(pid)` → ref | one-way watch; watcher receives `{'DOWN', ref, 'process', pid, reason}` (5-tuple, Erlang-shaped) on death. `demonitor(ref)` to cancel |
| `exit_proc(pid, reason)` | targeted exit signal — `'normal'` / `'killed'` / other atom |
| `trap_exit('true' \| 'false')` | toggle exit-signal trapping; on, exit signals arrive as `{'EXIT', from, reason}` messages |
| `supervise(strategy, [{name, fn, restart}, …])` | restart-tree supervisor (`'one_for_one'` / `'one_for_all'` / `'rest_for_one'`) — children fixed at start |
| `dyn_supervisor()` / `dyn_supervisor(max_restarts, max_seconds)` → pid | dynamic supervisor — starts empty, always `one_for_one`, add children at runtime |
| `sup_start_child(sup, {name, fn, restart})` → pid | add+start one supervised child at runtime (`name` may be `nil` for anonymous); `nil` on failure |
| `sup_terminate_child(sup, child)` → `'ok'` \| `'error'` | kill and forget one dynamic child |
| `sup_count_children(sup)` → int | number of live supervised children |

> **Compiled-only.** The process/scheduler primitives (spawn, send, receive, the supervisors, `process_list`/`process_info`/`registered` / `Swarm.*`, and the `tool_*` registry below) need the real scheduler / embedded interpreter, which the REPL path doesn't run. Under `swc run`, `swc test`, and the REPL they print a one-line note and return `nil` — run `swc build file.sw -o bin/x && bin/x` for full semantics.

### Tool registry — self-defined, hot-loaded tools
An agent can write a new tool *as `sw` source at runtime* and call it live, with no restart — the source is parsed and run by the interpreter that ships inside every compiled binary. The tool's entry function is `run`.

| | |
|---|---|
| `tool_define(name, src, caps?)` → `'ok'` \| `{'error', reason}` | parse `src` (a module defining `fun run(...)`), **admission-lint it** (reject hallucinated/undefined-fn calls and dangerous builtins not in `caps` — a list of capability atoms like `['db','file','shell']`; default = pure-logic only), and register under `name`. Re-defining hot-swaps + keeps the old for rollback |
| `tool_call(name, args…)` → result \| `nil` | run the tool's `run` with the trailing args (`nil` if no such tool) |
| `tool_list()` → `[{name, version}, …]` | every registered tool and its version |
| `tool_rollback(name)` → `'ok'` \| `{'error', reason}` | swap a tool back to its previous version (toggles) |
| `tool_history(name)` → `[{version, src}, …]` | every defined version of the tool as replayable source (last 16), oldest→newest — the audit log |

```sw
tool_define("summarize", "module T\nfun run(text) { ... }")
tool_call("summarize", article)
```

Tools are pure logic — process primitives degrade to `nil` inside them, and a tool keeps no state of its own (put state in the caller, ETS, or SQLite). Bad source fails loudly (`{'error', reason}`), never a silent `nil`. Compiled-only (see the note above).

### Subprocesses (bidirectional)
| | |
|---|---|
| `subprocess_spawn(cmd)` → handle | fork/exec with pipes for stdin + stdout |
| `subprocess_send_line(h, line)` | append `\n` if missing, write to child stdin |
| `subprocess_recv_line(h, timeout_ms?)` | block up to timeout for one newline-terminated line |
| `subprocess_close(h)` | close pipes, SIGTERM then SIGKILL, reap |

### SQLite
| | |
|---|---|
| `db_open(path)` → handle | `:memory:` works |
| `db_exec(h, sql)` | `'ok'` or error string. DDL or no-result statements |
| `db_exec(h, sql, [args])` | 3-arg form: prepares + **binds** `?` params + steps. Always bind user data this way — never string-interpolate into SQL |
| `db_query(h, sql, [args])` | `?` parameters; returns list of `%{col: value}` row maps |
| `db_close(h)` | `'ok'` |

### Sandboxed shell
| | |
|---|---|
| `shell_sandboxed(cmd, opts?)` | `{exit_code, output}` tuple. Wraps `cmd` in `sandbox-exec` (macOS) or `firejail` (Linux). Returns `nil` if no sandbox tool is available (deliberate — refuses to silently un-sandbox). Default policy: allow default, deny network, deny writes outside `/tmp`. |

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
| `Cron.every(ms, fn)` / `Cron.in_ms(ms, fn)` / `Cron.at("HH:MM", fn)` | timer-driven (import `Cron`); `after` is a `receive` keyword so the one-shot is named `in_ms` |
| `pubsub_subscribe(topic)` / `pubsub_broadcast(topic, msg)` / `pubsub_unsubscribe(topic)` | pub/sub |
| `breaker_new` / `breaker_call` / `breaker_state` | circuit breaker |

### PDF
| | |
|---|---|
| `pdf_text(path)` | extract text |
| `pdf_pages(path)` | int page count |
| `pdf_meta(path)` | metadata map |

### Verified builtin behavior

This is a runnable doctest — `scripts/doctest.sh` compiles, runs it, and
asserts each `# =>` line matches the actual stdout, so these claims can
never silently rot:

```sw
module BuiltinDemo
import Math
export [main]

fun main() {
    # Numbers & type checks
    print(abs(0 - 5))                 # => 5
    print(ord("A"))                   # => 65
    print(typeof(%{a: 1}))            # => map
    print(is_list([1, 2]))            # => :true
    print(is_map([1, 2]))             # => :false

    # Map default + new
    print(map_get(%{a: 1}, 'b', 99))  # => 99
    print(map_new())                  # => %{}

    # Bytes from ints
    print(bytes_from_ints([72, 73]))           # => <<72,73>>
    print(bytes_to_string(bytes_from_ints([72, 73])))  # => HI

    # ETS list shape is {k, v} tuples
    t = ets_new()
    ets_put(t, "a", 1)
    print(ets_list(t))                # => [{a, 1}]
    print(ets_count(t))               # => 1

    # Math (import Math)
    print(Math.sqrt(16))              # => 4
    print(Math.floor(3.7))            # => 3
    print(Math.max(2, 9))             # => 9

    # format() uses positional {} placeholders; f-strings interpolate
    # expressions inline as {expr} (and #{expr}).
    who = "ada"
    reqs = 3
    print(format("hi {} ({} req)", who, reqs))   # => hi ada (3 req)
    print(f"hi {who} #{reqs}")                    # => hi ada #3
}
```

`http_request(url, opts)` returns a status-aware map (`%{status, body, headers}`)
on a completed transport, or a tagged `{'error, reason}` when the request never
completed. This doctest exercises the error path deterministically (a dead port,
no server, no network), proving the tagged shape — see `tests/sw/test_http_request.sw`
for the full status + header + body round-trip against a live `http_listen` server:

```sw
module HttpReqDemo
export [main]

fun main() {
    # Port 9 (discard) with nothing listening → transport failure, tagged.
    case http_request("http://127.0.0.1:9/none", %{method: "GET"}) {
        {'error', _reason} -> print("transport_error")   # => transport_error
        resp               -> print(map_get(resp, "status"))
    }
}
```

---

## 11. Errors and panics

sw has two failure modes — choose by recoverability.

### Recoverable failures: `error` + `try / catch`

```sw
r = try {
    raw = file_read("/etc/some.conf")
    if (raw == nil) { error("config missing") }
    parse(raw)
} catch e {
    f"using defaults — {e}"
}
```

`error(msg)` sets a thread-local error sentinel that `try { ... } catch e { ... }` catches. Outside a `try`, `error()` is silent (the calling code continues with `nil`), which makes try/catch the explicit "I want to handle failure" marker.

### Unrecoverable failures: `panic` + `expect`

```sw
panic("invariant violated: queue should never be empty here")

name = expect(map_get(user, 'name'), "user record missing required 'name' field")
```

`panic(msg)` prints a red `panic: <msg>` plus the exact `src/Mod.sw:LINE` where it fired AND the full call chain (innermost first), then exits with code 1. Cannot be caught. Use for programmer bugs (impossible states, broken invariants).

In `swc test` files, use `assert_raises(fn, expected_msg)` to assert that a zero-arg lambda panics (or calls `error()`) with a message containing `expected_msg` — the test runner intercepts the panic before it reaches `exit(1)` so the suite continues.

```
panic: hit the bottom
  at src/Trace.sw:4
  call chain (innermost first):
    [0] Trace.deep at src/Trace.sw:4
    [1] Trace.middle at src/Trace.sw:12
    [2] Trace.outer at src/Trace.sw:8
    [3] Trace.main at src/Trace.sw:16
```

`expect(value, msg)` is the idiomatic "unwrap" pattern — passes the value through if non-nil, otherwise panics with `msg`. Saves the explicit `if (x == nil) { panic(...) }` boilerplate.

### Builtins that panic (instead of returning nil silently)

| Builtin | Panics when |
|---|---|
| `hd(lst)` | `lst` is empty or not a list |
| `tl(lst)` | `lst` is empty or not a list |
| `elem(t, i)` | `i` is out of range or `t` is not a tuple |
| `n / 0`, `n % 0` | divisor is zero |

`map_get(m, k)` and `ets_get(t, k)` stay lenient — `nil` for missing keys is intentional ("optional lookup"). Pair them with `expect()` when the key MUST be present.

### Compile-time "did you mean?"

`swc` prints a hint when you call a function it can't resolve:

```
src/Hello.sw:4: unknown function 'strng_length' — did you mean 'string_length'?
```

Levenshtein-distance suggestions over the builtin list plus the calling module's functions.

---

## 12. Known gotchas

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

- **Userland fault tolerance is FULL.** `link`, `unlink`, `monitor`, `demonitor`, `exit_proc(pid, reason)`, `trap_exit('true' | 'false')` are all wired. The classic OTP "let it crash" pattern works: spawn supervised children, trap_exit, receive `{'EXIT', from, reason}` and restart. (Previously this was listed as missing — landed 2026-05-20.)

- **The runtime exits when `main()` returns.** Go-style, not Erlang-style — when your program's `main` function finishes, the runtime tears down the schedulers and the process exits with code 0. If you want a long-running server, end `main` with a permanent `receive { _other -> ... }` arm or `sleep(N)` loop. (Until 2026-05-15 the generated `main()` ran an infinite `usleep` loop, so every program hung after main finished. That's now fixed.)

- **Map literal `%{` is a SINGLE TOKEN.** `expr % {…}` does NOT parse as "modulo of expr by a brace block" — `%` and `{` need a space if you want modulo of `expr` by some expression starting with `{` (which is uncommon anyway). Use `expr % some_var` for clarity.

- **`++` is polymorphic** — strings concat, lists concat. `"foo" ++ "bar"` → `"foobar"`, `[1] ++ [2, 3]` → `[1, 2, 3]`. (Was string-only until 2026-05-20.)

- **`map_get` treats atom and string keys as the same** when values match by text. So `map_get(json_decode("{\"x\":1}"), 'x')` finds the string-keyed `"x"` — JSON-decoded maps interop with atom-keyed sw literals.

- **`after N` in receive timed out correctly since 2026-05-20.** Earlier the internal timer-wake confused the wait loop and the receive spun forever. The codegen now tracks elapsed time at the receive level. `after VAR` (dynamic timeout) also works now.

- **Pattern `nil`** matches BOTH the runtime nil value AND the literal atom `'nil'`. They were treated as different in pattern position until 2026-05-20 — `case map_get(...) { nil -> ... }` silently missed when the value was actually nil.

- **Module functions are first-class values.** `%{handler: my_fn}` stores a callable closure to `my_fn`, not the atom `'my_fn'`. (Was stored as atom until 2026-05-20.)

---

## 13. Build pipeline

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

# Run a .sw file directly without producing a binary (interpret + JIT)
bin/swc run my_program.sw

# Run sw interpreter tests (point at a file or directory of test_*.sw files)
bin/swc test

# Start the Language Server Protocol server (for editor integration)
bin/swc lsp
```

### `swc build` — ahead-of-time compilation

Emits C to `/tmp/swc_<Module>_*.c`, then invokes `cc` linking against `libswarmrt.a`. Errors at the C step now include `#line` directives so they point back at sw source. If swc itself errors at the parse stage, the message includes the sw line directly.

### `swc run` — direct execution

Interprets and runs a `.sw` file without writing a binary to disk. Useful for scripts and quick iteration. Slower startup than a pre-built binary but avoids the build step entirely.

### `swc test` — test runner

Runs sw test files through the tree-walking interpreter. Point it at a single file or a directory containing `test_*.sw` / `*_test.sw` files. Prints a per-test pass/fail summary. A non-zero exit code means at least one test failed.

```bash
# Example output
bin/swc test tests/sw/repl/test_repl_builtins_interp.sw
#   16 tests, 16 passed (8.9ms)
```

The broader test suite (`make test-sw`) compiles and runs the 8 files under `tests/sw/` (110 sw assertions total). The C-side phase regression tests (75 tests across phases 2–10) run via `make test-phase{2..10}` or `make test-full` — they are separate from `swc test`.

Inside your own `.sw` test files, use `assert_raises(fn, expected_msg)` to assert that a zero-arg lambda panics or errors with a message containing `expected_msg`. The test runner intercepts the panic before it hits `exit(1)` so the suite continues running.

### `swc lsp` — language server

Starts a Language Server Protocol server on stdio, compatible with any LSP-aware editor (VS Code, Neovim, Helix, etc.). Provides go-to-definition, hover docs, and diagnostics backed by the tree-sitter grammar.

Don't `rm -rf bin/` in the swarmrt dir — that wipes `bin/swc` and `bin/libswarmrt.a`. Use `make swc libswarmrt` to rebuild after a clean.
