module Test_async_builtins

# Phase 2e (docs/design/PROCESS_WEIGHT.md): builtins that wait park a
# spawned process without holding a stack. sleep parks until its deadline
# (mail that arrives meanwhile wakes it, stays queued, and it parks again);
# the offloaded builtins (shell, exec_*, http_*, llm_complete) park until
# their job on the offload pool is done; map, filter and reduce resume their
# loop after a callback that waited. Each case runs in a spawned process,
# and the last one parks 40,000 processes inside a map callback, past the
# ~32,700 stacks stock Linux can map.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun sleeper(parent) {
    t0 = timestamp()
    r = sleep(150)
    took = timestamp() - t0
    # The 'poke' sent mid-sleep is still queued.
    poked = receive { 'poke' -> 'kept' after 0 { 'lost' } }
    send(parent, {'sleep', {r, took >= 150, poked}})
}

fun shell_user(parent) {
    r = shell("echo async-shell")
    e = exec_argv("sh", ["-c", "echo $((6 * 7))"])
    send(parent, {'shell', {string_trim(elem(r, 1)), string_trim(elem(e, 1))}})
}

fun map_waits(parent) {
    me = self()
    r = map(fn(x) { send(me, {'echo', x * 10}) ; receive { {'echo', v} -> v + 1 } }, [1, 2, 3])
    send(parent, {'map', r})
}

fun filter_sleeps(parent) {
    r = filter(fn(x) { sleep(5) ; x % 2 == 0 }, [1, 2, 3, 4, 5, 6])
    send(parent, {'filter', r})
}

fun reduce_waits(parent) {
    me = self()
    r = reduce(fn(acc, x) { send(me, {'v', x}) ; receive { {'v', v} -> acc + v } }, [1, 2, 3, 4], 100)
    send(parent, {'reduce', r})
}

fun nested_maps(parent) {
    r = map(fn(row) { map(fn(x) { sleep(1) ; x * row }, [1, 2]) }, [1, 2, 3])
    send(parent, {'nested', r})
}

fun piped(parent) {
    r = [1, 2, 3] |> map(fn(x) { sleep(1) ; x + 1 })
    send(parent, {'piped', r})
}

fun map_raises(parent) {
    r = try { map(fn(x) { sleep(1) ; if (x == 2) { error("two") } else { x } }, [1, 2, 3]) } catch e { e }
    send(parent, {'raise', r})
}

fun pmap_sleeps(parent) {
    r = pmap(fn(x) { sleep(20) ; x * 2 }, [1, 2, 3, 4, 5])
    send(parent, {'pmap', r})
}

# A process killed while its shell command runs on the offload pool; the
# pool keeps working for the next caller.
fun long_shell(parent) { send(parent, {'started', self()}) ; shell("sleep 5") ; send(parent, {'long', 'finished'}) }
fun killed_mid_offload(parent) {
    me = self()
    spawn(long_shell(me))
    p = receive { {'started', pid} -> pid after 5000 { nil } }
    sleep(100)
    exit_proc(p, 'kill')
    r = shell("echo still-works")
    send(parent, {'killed', string_trim(elem(r, 1))})
}

# 40,000 processes parked inside a map callback.
fun map_waiter(parent) {
    r = map(fn(x) { receive { 'go' -> x + 1 } }, [41])
    send(parent, {'woke', r})
}
fun spawn_waiters(n, me, acc) { if (n == 0) { acc } else { spawn_waiters(n - 1, me, [spawn(map_waiter(me)) | acc]) } }
fun count_woke(n, acc) {
    if (n == 0) { acc }
    else { receive { {'woke', [42]} -> count_woke(n - 1, acc + 1) after 10000 { acc } } }
}

fun await(tag) { receive { {t, v} when t == tag -> v after 10000 { 'timeout' } } }

fun main() {
    me = self()
    f = 0
    s = spawn(sleeper(me))
    sleep(50)
    send(s, 'poke')
    f = f + assert_eq("sleep parks, keeps mail and lasts its time", await('sleep'), {'ok', 'true', 'kept'})
    spawn(shell_user(me))
    f = f + assert_eq("shell and exec_argv park on the offload pool", await('shell'), {"async-shell", "42"})
    spawn(map_waits(me))
    f = f + assert_eq("map callback that receives", await('map'), [11, 21, 31])
    spawn(filter_sleeps(me))
    f = f + assert_eq("filter callback that sleeps", await('filter'), [2, 4, 6])
    spawn(reduce_waits(me))
    f = f + assert_eq("reduce callback that receives", await('reduce'), 110)
    spawn(nested_maps(me))
    f = f + assert_eq("nested maps that sleep", await('nested'), [[1, 2], [2, 4], [3, 6]])
    spawn(piped(me))
    f = f + assert_eq("piped map that sleeps", await('piped'), [2, 3, 4])
    spawn(pmap_sleeps(me))
    f = f + assert_eq("pmap workers and collector that park", await('pmap'), [2, 4, 6, 8, 10])
    f = f + assert_eq("pmap from main (blocking path)", pmap(fn(x) { sleep(5) ; x + 1 }, [1, 2, 3]), [2, 3, 4])
    spawn(killed_mid_offload(me))
    f = f + assert_eq("kill while offloaded, pool still serves", await('killed'), "still-works")
    spawn(map_raises(me))
    f = f + assert_eq("error in a map callback after it parked", await('raise'), "two")

    ws = spawn_waiters(40000, me, [])
    sleep(300)
    for w in ws { send(w, 'go') }
    f = f + assert_eq("40,000 processes parked inside a map callback", count_woke(40000, 0), 40000)

    if (f == 0) { print("OK test_async_builtins all") } else { panic("async builtin tests failed") }
}
