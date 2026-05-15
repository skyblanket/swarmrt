# Writing `sw` — for AI Agents

This is the document to load into a model's context before pointing it at SwarmRT. It is short on purpose. The goal is that an LLM with this in its system prompt and `SW_LANGUAGE.md` available on demand can write working `.sw` code on the first try.

If you are a human dev, you probably want [SW_LANGUAGE.md](SW_LANGUAGE.md) instead — it's the full reference. This file is the *cheat sheet*.

---

## The shape of a sw program

```sw
module Hello
export [main]

fun main() {
    print("hello, swarm")
}
```

- One `module` per file. Module name is CamelCase.
- `export` lists functions visible to other modules.
- Functions are defined with `fun name(args) { body }`.
- Files live at `src/<ModuleName>.sw` (or `src/main.sw` for `module Main`).
- Imports are auto-resolved — write `import OtherModule` and the compiler finds `src/OtherModule.sw`.

To compile: `swc build src/main.sw -o myprog && ./myprog`.

---

## The shape of a sw process

This is the canonical pattern. Internalise it.

```sw
fun worker_loop(state) {
    receive {
        {'do_thing', arg, from} ->
            new_state = handle_thing(state, arg)
            send(from, {'result', new_state})
            worker_loop(new_state)

        {'update', new_state} ->
            worker_loop(new_state)

        'stop' -> 'ok'
    }
}

fun start_worker() {
    spawn(worker_loop(initial_state()))
}
```

Six things to notice:

1. **A process is a function that calls itself in tail position.** sw's compiler turns `worker_loop(...)` in tail position into a `goto` — your stack does not grow, and the process can run forever.
2. **State is the recursion argument.** No mutable variables. To "update" state, recurse with new values.
3. **`receive { ... }` blocks until a matching message arrives.** Patterns are matched in order. `_other -> ...` catches anything (use sparingly).
4. **Atoms (`'stop'`, `'do_thing'`) are tags.** They are interned strings the runtime compares by pointer. Use them for message tags, status codes, sentinels.
5. **Tuples (`{...}`) are how you bundle a tag with data.** `{'do_thing', arg, from}` is the standard request shape: `{tag, payload, reply_pid}`.
6. **`spawn(expr)` evaluates `expr` in a new process and returns its pid.** The pid is what you `send` to.

---

## Talking between processes

```sw
# Async fire-and-forget
send(pid, {'log', "hello"})

# Request/reply — caller waits for a response tagged with their pid
send(pid, {'compute', 42, self()})
result = receive {
    {'done', value} -> value
    after 5000 { 'timeout' }
}
```

`self()` is your own pid. `after N` is a timeout in ms — the receive returns whatever the after-block evaluates to.

---

## Selective receive (this is the superpower)

A `receive` will only consume messages that match one of its patterns. Anything else stays in the mailbox for the next `receive` to look at. This means a process waiting for a specific reply doesn't have to handle every other message that comes in:

```sw
# Wait specifically for a 'done' message tagged with our request_id
fun wait_for(request_id) {
    receive {
        {'done', id, value} when id == request_id -> value
        # Other messages get re-queued by the runtime, not by us
    }
}
```

This is why processes are the right granularity for AI agents — each agent can wait for *its* tool reply without being woken up by every other agent's traffic.

---

## ETS — shared mutable state, when you need it

Maps and lists are immutable. When you need a counter, a registry, or a cache shared between processes, use ETS:

```sw
table = ets_new()
ets_put(table, 'counter', 0)
ets_put(table, 'counter', ets_get(table, 'counter') + 1)

# Pass the table value into a spawned process — every process sharing
# the value sees the same backing storage.
spawn(worker_loop(table))
```

`ets_get` returns `nil` for missing keys. `ets_delete(table, 'k')` removes one. `ets_count(table)` is the size.

---

## The most useful builtins

```sw
# Strings
string_length(s)            string_sub(s, start, len)
string_split(s, sep)        string_contains(s, needle)   # 'true' / 'false'
string_starts_with(s, p)    string_ends_with(s, p)
string_replace(s, a, b)     string_trim(s)
string_upper(s)             string_lower(s)
string_index_of(hay, needle)   # int, -1 if not found

# JSON
json_encode(value)          # → string
json_decode(string)         # → value or nil

# Base64 (binary safe)
base64_encode(s)            base64_decode(s)

# HTTP
http_get(url, headers)      # headers is [{"Header", "value"}, ...]
http_post(url, headers, body)
# Both return the response body as a string, or nil on failure.

# Files
file_read(path)             file_write(path, content)
file_exists(path)           file_delete(path)
file_list(dir)              # → list of names

# Time
timestamp()                 # ms since epoch
sleep(ms)

# Process / messaging
spawn(expr)   send(pid, msg)   self()   register('name', pid)   whereis('name')

# ETS
ets_new()  ets_put(t, k, v)  ets_get(t, k)  ets_delete(t, k)  ets_list(t)  ets_count(t)

# Shell + env
shell(cmd)        # → {exit_code, output}
getenv(name)      sys_exit(code)

# Print / read
print(text)       print_inline(text)        # no newline
read_line(prompt) read_char()
```

The full builtin index is in [SW_LANGUAGE.md](SW_LANGUAGE.md).

---

## Pattern matching cheatsheet

Inside `receive` arm patterns:

| Pattern | Matches |
|---|---|
| `42` | the integer 42 |
| `"hello"` | the string `"hello"` |
| `'ok'` | the atom `'ok'` |
| `{'tag', x, y}` | a 3-tuple where `'tag'` matches literally; `x` and `y` bind |
| `{'reply', n} when n > 0` | tuple AND guard; `n` must be positive |
| `[h \| t]` | list with at least one element; `h` is head, `t` is tail |
| `_anything` | binds to `_anything` — use as catchall |

Variables in patterns *bind*; atoms (single-quoted) match *literally*. That's the entire matching system.

---

## Things that bite

These are the gotchas you will hit if you're new. The full list is in [SW_LANGUAGE.md](SW_LANGUAGE.md), but these three trip up models the most:

**1. There are no mutable variables — recurse.**

```sw
# WRONG — looks like Python, doesn't work
fun count_loop(n) {
    while (n < 10) { n = n + 1 ; print(n) }   # no `while`, no rebinding
}

# RIGHT — tail-recursive process
fun count_to(n, target) {
    if (n >= target) { 'done' }
    else { print(n) ; count_to(n + 1, target) }
}
```

**2. Strings concatenate with `++`, not `+`.**

```sw
"hello, " ++ name        # right (joins strings)
"hello, " + name         # wrong — `+` is for numbers
"count: " ++ n           # also right — ++ auto-coerces ints, floats, atoms
```

**3. `print` takes any number of args, joined with spaces. No `to_string` needed for prose.**

```sw
print("count:", n)                # → "count: 8"
print("name:", name, "age:", n)   # → "name: alice age: 30"
print("count: " ++ n)             # also fine — same output
```

**4. The runtime exits when your `main()` returns.** If you spawned workers and want them to keep running, end `main` with a permanent `receive { _other -> ... }` or a `sleep(very_big)` loop. (Go-style, not Erlang-style.)

---

## A complete agent-shaped example

```sw
module AgentExample
export [main, agent_loop]

# An agent: receives tasks, calls an LLM, replies.
fun agent_loop(name, history) {
    receive {
        {'ask', prompt, reply_to} ->
            new_history = list_append(history, {'user', prompt})
            response = call_llm(new_history)
            send(reply_to, {'agent_reply', name, response})
            agent_loop(name, list_append(new_history, {'assistant', response}))

        'reset' ->
            agent_loop(name, [])

        'stop' ->
            'done'
    }
}

fun call_llm(messages) {
    body = json_encode(%{model: "kimi-k2.6", messages: messages})
    hdrs = [{"Authorization", "Bearer " ++ getenv("API_KEY")},
            {"Content-Type", "application/json"}]
    resp = http_post("https://api.moonshot.ai/v1/chat/completions", hdrs, body)
    if (resp == nil) { "[llm error]" }
    else {
        decoded = json_decode(resp)
        choices = map_get(decoded, 'choices')
        msg = map_get(hd(choices), 'message')
        map_get(msg, 'content')
    }
}

fun main() {
    pid = spawn(agent_loop("scout", []))
    send(pid, {'ask', "What's 2+2?", self()})

    receive {
        {'agent_reply', name, content} ->
            print("[" ++ name ++ "] " ++ to_string(content))
    }

    send(pid, 'stop')
}
```

This is a complete LLM-talking concurrent agent in 35 lines, with no external dependencies, that compiles to a single ~400KB native binary.

---

## When you get stuck

1. **Compile error pointing at C code?** Look for the `src/<Module>.sw:N: error` line — the codegen emits `#line` directives so the C compiler's error tells you the exact `.sw` line that's wrong.

2. **Parse error?** Count `{` vs `}` in the failing function. Most parse errors are mis-balanced braces.

3. **Process never receives a message?** Check that the *sender* has the right `pid`. `whereis('name')` returns `nil` if the name isn't registered yet.

4. **Need to debug message flow?** Stick `print("got: " ++ to_string(msg))` in your receive arms. Atoms and tuples print readably.

5. **Real reference:** [SW_LANGUAGE.md](SW_LANGUAGE.md) for the full language. [API_REFERENCE.md](API_REFERENCE.md) for the C runtime if you're embedding swarmrt.

---

## Recommended system-prompt snippet

If you're embedding sw-writing capability into an agent, this snippet (paste into the system prompt) is enough for most tasks:

> You are writing `.sw` code for the SwarmRT runtime. The full language reference is at `docs/SW_LANGUAGE.md`. Key facts:
> - Modules begin with `module Name` and `export [list, of, fns]`. Files live at `src/<ModuleName>.sw`.
> - Processes are tail-recursive functions that call themselves in `receive { pattern -> body ; recur(state) }`.
> - State is immutable — recurse with new values to "update" it. Use `ets_new()` + `ets_put`/`ets_get` for shared mutable state.
> - Strings concatenate with `++`, not `+`. `++` auto-coerces numbers/atoms to strings. `print(...)` is variadic and joins args with spaces.
> - Atoms (`'ok'`, `'error'`) are message tags. Tuples (`{...}`) bundle them with data: `{'tag', payload, reply_pid}` is the convention for request/reply.
> - Statements separate by newline OR `;` — both work in any block.
> - Compile with `swc build src/main.sw -o myprog && ./myprog`.
> - When in doubt, mirror the patterns in `examples/`.

That's the document. Build something cool.
