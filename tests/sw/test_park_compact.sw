module Test_park_compact

# Phase 2f (docs/design/PROCESS_WEIGHT.md §3.7): a process that parks has its
# live values copied into a fresh arena and the rest freed. Whatever it can
# still reach must survive: values in its frames (a map, a list, a closure
# and its captures, shared substructure), loop state carried through a
# self-tail call, a comprehension or a map half built when its body parked,
# and pmap's collector. Run with SW_COMPACT_TORTURE=1 to compact at every
# park, yields included (make test-sw-compact).

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun garbage(n, acc) { if (n == 0) { length(acc) } else { garbage(n - 1, [to_string(n) ++ "-junk" | acc]) } }

fun tick() { receive { 'tick' -> 'ok' } }

# Live values across many parks, with garbage made between them.
fun holder(parent) {
    shared = ["s", 1]
    m = %{name: "alice", tags: ["a", "b"], pair: {shared, shared}}
    xs = [1, 2, 3, "four", {5, 6}]
    k = 10
    f = fn(y) { y + k + length(xs) }
    r = hold_loop(20, m, xs, f)
    send(parent, {'held', {r, map_get(m, 'name'), elem(map_get(m, 'pair'), 0), f(1), xs}})
}
fun hold_loop(n, m, xs, f) {
    if (n == 0) { 'done' }
    else {
        garbage(200, [])
        tick()
        if (map_get(m, 'tags') == ["a", "b"] && f(0) == 15 && length(xs) == 5) {
            hold_loop(n - 1, m, xs, f)
        } else { 'corrupt' }
    }
}

# Loop state carried through a self-tail call that parks every turn.
fun counter(parent, n, acc) {
    if (n == 0) { send(parent, {'count', acc}) }
    else {
        junk = garbage(100, [])
        v = receive { {'add', x} -> x }
        counter(parent, n - 1, [v + junk - junk | acc])
    }
}

# A comprehension and a map whose body parks halfway through.
fun builders(parent) {
    me = self()
    c = [receive { {'item', v} -> v * 2 } for x in [1, 2, 3, 4] when send(me, {'item', x}) != nil]
    m = map(fn(x) { send(me, {'m', x}) ; garbage(50, []) ; receive { {'m', v} -> {v, "x"} } }, [7, 8])
    send(parent, {'built', {c, m}})
}

fun pmapper(parent) {
    r = pmap(fn(x) { sleep(10) ; garbage(50, []) ; [x, x * x] }, [1, 2, 3])
    send(parent, {'pmapped', r})
}

fun await(tag) { receive { {t, v} when t == tag -> v after 10000 { 'timeout' } } }

fun pump(p, n) { if (n == 0) { 'ok' } else { sleep(2) ; send(p, 'tick') ; pump(p, n - 1) } }
fun feed(p, i, n) { if (i > n) { 'ok' } else { sleep(1) ; send(p, {'add', i}) ; feed(p, i + 1, n) } }

fun main() {
    me = self()
    f = 0
    h = spawn(holder(me))
    pump(h, 20)
    f = f + assert_eq("live map, list and closure survive parks",
                      await('held'), {'done', "alice", ["s", 1], 16, [1, 2, 3, "four", {5, 6}]})
    c = spawn(counter(me, 30, []))
    feed(c, 1, 30)
    f = f + assert_eq("loop state through a parking self-tail call",
                      await('count'), [30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16,
                                       15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1])
    spawn(builders(me))
    f = f + assert_eq("half-built comprehension and map", await('built'),
                      {[2, 4, 6, 8], [{7, "x"}, {8, "x"}]})
    spawn(pmapper(me))
    f = f + assert_eq("pmap collector across parks", await('pmapped'), [[1, 1], [2, 4], [3, 9]])
    if (f == 0) { print("OK test_park_compact all") } else { panic("park compaction tests failed") }
}
