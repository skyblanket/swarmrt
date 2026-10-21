module Test_stackless

# Phase 2c (docs/design/PROCESS_WEIGHT.md): a spawned compiled process parks
# by saving its frames to the heap and giving its stack back. Every way a
# parked activation can resume must deliver the same values a stackful run
# would: a receive deep in a call chain, a callee's value to its caller, a
# closure call, a mutual tail call after a resume, a receive deadline that
# expires after a resume, and a loop that yields on reductions.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# A receive three calls deep; each caller adds to the callee's value.
fun leaf() { receive { {'v', x} -> x } }
fun mid() { leaf() * 10 + 1 }
fun top(parent) { send(parent, {'deep', mid() + 100}) }

# A closure that waits, called through a variable.
fun via_closure(parent) {
    wait = fn(tag) { receive { {t, x} when t == tag -> x } }
    a = wait('a')
    b = wait('b')
    send(parent, {'closure', a * 100 + b})
}

# Mutual tail calls across receives: ping waits, then hops to pong.
fun ping(n, acc, parent) {
    if (n == 0) { send(parent, {'hops', acc}) }
    else { receive { {'tick', x} -> pong(n, acc + x, parent) } }
}
fun pong(n, acc, parent) { ping(n - 1, acc * 2, parent) }

# A deadline that expires while parked.
fun timed(parent) {
    r = receive { 'never' -> 'wrong' after 30 { 'timed_out' } }
    send(parent, {'timed', r})
}

# A long loop that only yields on reductions.
fun spin(i, acc) { if (i == 0) { acc } else { spin(i - 1, acc + i) } }
fun spinner(parent) { send(parent, {'spin', spin(200000, 0)}) }

fun await(tag) { receive { {t, v} when t == tag -> v after 5000 { 'timeout' } } }

fun main() {
    me = self()
    f = 0

    d = spawn(top(me))
    sleep(20)
    send(d, {'v', 7})
    f = f + assert_eq("receive three calls deep", await('deep'), 171)

    c = spawn(via_closure(me))
    sleep(10)
    send(c, {'b', 2})
    sleep(10)
    send(c, {'a', 3})
    f = f + assert_eq("closure that waits", await('closure'), 302)

    p = spawn(ping(4, 0, me))
    for i in [1, 2, 3, 4] { sleep(5)  send(p, {'tick', i}) }
    f = f + assert_eq("mutual tail calls across receives", await('hops'), 52)

    spawn(timed(me))
    f = f + assert_eq("deadline expires while parked", await('timed'), 'timed_out')

    spawn(spinner(me))
    f = f + assert_eq("loop that yields on reductions", await('spin'), 20000100000)

    if (f == 0) { print("OK test_stackless all") } else { panic("stackless tests failed") }
}
