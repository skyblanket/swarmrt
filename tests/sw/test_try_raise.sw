module Test_try_raise

# Phase 2d (docs/design/PROCESS_WEIGHT.md §3.6): try/catch without setjmp.
# error() inside a live try raises, and the raise unwinds every call between
# it and the nearest catch: callees, closures, pipes, comprehensions and
# callbacks called from C (map, filter, ets_update). Nothing after the error
# runs. A try body can park (receive, sleep, reductions) and still catch
# afterwards, in main and in spawned stackless processes, without holding a
# stack while parked. With no live try, error() returns nil and execution
# continues.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun boom(x) { error(x) ; "after error" }
fun deep(n) { if (n == 0) { boom("deep") ; 'unreached' } else { deep(n - 1) + 1 } }

fun in_callee() { try { boom("callee") ; 'unreached' } catch e { e } }
fun in_deep_callee() { try { deep(50) } catch e { e } }

fun in_lambda() {
    f = fn(x) { error("lambda " ++ to_string(x)) ; x }
    try { f(7) ; 'unreached' } catch e { e }
}

fun in_pipe() { try { "piped" |> boom() ; 'unreached' } catch e { e } }

fun in_comprehension() {
    try { [if (x == 3) { boom("comp " ++ to_string(x)) } else { x } for x in [1, 2, 3, 4]] } catch e { e }
}

# map stops calling the callback once it has raised.
fun in_map(t) {
    r = try {
        map(fn(x) { ets_put(t, x, 'seen') ; if (x == 2) { error("map 2") } else { x } }, [1, 2, 3, 4])
    } catch e { e }
    {r, ets_get(t, 3)}
}

fun in_filter() { try { filter(fn(x) { if (x > 1) { error("filter") } else { 'true' } }, [1, 2, 3]) } catch e { e } }

# A raising ets_update leaves the entry as it was.
fun in_ets_update(t) {
    ets_put(t, 'k', 10)
    r = try { ets_update(t, 'k', fn(v) { error("upd") ; v + 1 }) } catch e { e }
    {r, ets_get(t, 'k')}
}

# The statement after the error never runs.
fun stops_body(t) {
    r = try { error("stop") ; ets_put(t, 'ran', 'yes') ; 'unreached' } catch e { e }
    {r, ets_get(t, 'ran')}
}

fun nested() {
    try {
        inner = try { error("inner") } catch e { "caught " ++ e }
        error(inner ++ ", then outer")
    } catch e2 { e2 }
}

fun error_in_catch() {
    try { try { error("first") } catch e { error("from catch: " ++ e) ; 'unreached' } } catch e2 { e2 }
}

fun no_try() { r = error("nobody listens") ; if (r == nil) { 'continued' } else { 'wrong' } }

fun after_receive(parent) {
    r = try {
        v = receive { {'go', x} -> x }
        boom("after receive " ++ to_string(v))
        'unreached'
    } catch e { e }
    send(parent, {'recv', r})
}

fun after_sleep(parent) {
    r = try { sleep(10) ; boom("after sleep") } catch e { e }
    send(parent, {'sleep', r})
}

fun spin(i, acc) { if (i == 0) { acc } else { spin(i - 1, acc + 1) } }
fun after_spin(parent) {
    r = try { n = spin(100000, 0) ; boom("after " ++ to_string(n) ++ " turns") } catch e { e }
    send(parent, {'spin', r})
}

# A raise thrown from a frame that was saved to the heap and resumed.
fun parked_deep(n) { if (n == 0) { receive { 'go' -> boom("parked deep") } } else { parked_deep(n - 1) + 1 } }
fun parked_catch(parent) {
    r = try { parked_deep(20) } catch e { e }
    send(parent, {'parked', r})
}

# 40,000 processes parked in a receive inside a try. A try body used to keep
# its stack (setjmp), and stock Linux runs out of stack mappings near 32,700.
fun try_waiter(parent) {
    r = try { receive { 'go' -> error("woke") } } catch e { e }
    send(parent, {'woke', r})
}
fun spawn_waiters(n, me, acc) { if (n == 0) { acc } else { spawn_waiters(n - 1, me, [spawn(try_waiter(me)) | acc]) } }
fun count_woke(n, acc) {
    if (n == 0) { acc }
    else { receive { {'woke', "woke"} -> count_woke(n - 1, acc + 1) after 10000 { acc } } }
}

fun await(tag) { receive { {t, v} when t == tag -> v after 5000 { 'timeout' } } }

fun main() {
    t = ets_new()
    f = 0
    f = f + assert_eq("error in a callee", in_callee(), "callee")
    f = f + assert_eq("error 50 calls deep", in_deep_callee(), "deep")
    f = f + assert_eq("error in a lambda", in_lambda(), "lambda 7")
    f = f + assert_eq("error in a pipe", in_pipe(), "piped")
    f = f + assert_eq("error in a comprehension", in_comprehension(), "comp 3")
    f = f + assert_eq("error in a map callback stops map", in_map(t), {"map 2", nil})
    f = f + assert_eq("error in a filter callback", in_filter(), "filter")
    f = f + assert_eq("error in ets_update keeps the entry", in_ets_update(t), {"upd", 10})
    f = f + assert_eq("nothing after error runs", stops_body(t), {"stop", nil})
    f = f + assert_eq("nested tries", nested(), "caught inner, then outer")
    f = f + assert_eq("error in a catch goes to the outer try", error_in_catch(), "from catch: first")
    f = f + assert_eq("error with no try returns nil", no_try(), 'continued')

    me = self()
    p = spawn(after_receive(me))
    sleep(20)
    send(p, {'go', 5})
    f = f + assert_eq("error after a receive in a try", await('recv'), "after receive 5")
    spawn(after_sleep(me))
    f = f + assert_eq("error after a sleep in a try", await('sleep'), "after sleep")
    spawn(after_spin(me))
    f = f + assert_eq("error after reduction yields in a try", await('spin'), "after 100000 turns")
    q = spawn(parked_catch(me))
    sleep(20)
    send(q, 'go')
    f = f + assert_eq("error from a resumed heap frame", await('parked'), "parked deep")

    ws = spawn_waiters(40000, me, [])
    sleep(300)
    for w in ws { send(w, 'go') }
    f = f + assert_eq("40,000 processes parked inside a try", count_woke(40000, 0), 40000)

    if (f == 0) { print("OK test_try_raise all") } else { panic("try/raise tests failed") }
}
