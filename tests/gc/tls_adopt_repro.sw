# tls_repro.sw — deterministic repro for the cross-fiber adopt-TLS bug.
#
# THE BUG (pre-fix, now fixed): a C-level VALUE-message receive chose
# ADOPT-region-into-my-arena vs MATERIALIZE-a-free-able-global-copy by reading a
# scheduler-thread-local `g_recv_adopt`. That flag was set to 1 by
# sw_recv_any_adopt (used by pmap) AROUND a BLOCKING receive that
# context-switches fibers. So while one fiber (a pmap driver) is parked inside
# sw_recv_any_adopt with adopt=1 still live in TLS, ANOTHER fiber scheduled on
# that same OS thread runs its OWN C-level VALUE receive and inherits adopt=1 —
# the wrong decision: it ADOPTS the message region (returns a pointer INTO an
# arena chunk) instead of materializing a free-able global copy. The consumer
# then free()s that pointer -> free of a non-malloc, arena-interior address ->
# heap corruption, caught by ASan (+ -DSW_ARENA_POISON).
#
# THE C-LEVEL FREE-ING CONSUMER we exploit: `sleep(ms)` compiles to
# _builtin_sleep (swarmrt_builtins_studio.h), which is literally
#     void *msg = sw_receive_any(ms, &tag);  if (msg) free(msg);
# i.e. a PLAIN sw_receive_any (materialize+free path) that free()s whatever it
# pops. If a VALUE message lands in a process while it is sleeping AND a pmap on
# the same scheduler thread has leaked adopt=1, _builtin_sleep frees an
# arena-interior pointer. (Compiled `receive` is immune: it has its own
# adopt-splice loop and never calls sw_receive_any.)
#
# DETERMINISTIC INTERLEAVE (run under SW_SCHEDULERS=1 — one OS thread, one runq,
# cooperative fibers):
#   * `sleeper(coord)` registers itself, then sleep()s for a long window. Inside
#     that sleep it is parked in sw_receive_any (the free path), waiting.
#   * Many `driver` processes each call pmap() whose workers sleep() so the
#     driver BLOCKS in sw_recv_any_adopt (g_recv_adopt=1 live in TLS, pre-fix).
#   * A `poker` process sends a fat VALUE message (tuple of list+map+string,
#     carried in a MESSAGE region) to the sleeper WHILE the drivers are mid-pmap.
#     On the single shared thread the sleeper's sw_receive_any pops that VALUE
#     message while a driver's adopt=1 is still set -> _builtin_sleep frees an
#     arena-interior pointer -> ASan fault on the PRE-FIX build.
#   * We send MANY VALUE messages across MANY pokes/drivers to make the
#     interleave hit reliably regardless of exact pick order.
#
# PASS on the FIXED build (adopt is a stack parameter, never visible to another
# fiber): the sleeper's sw_receive_any always materializes a free-able global
# copy, free() is safe, every pmap result is correct, exit 0.

module Main

fun assert_eq(name, actual, expected) {
    if (actual == expected) { 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# A fat compound VALUE — forces a real MESSAGE region (RAW payloads are immune
# to the adopt/materialize decision).
fun fat(x) {
    {'payload', x, [x, x * 2, x * 3, "s" ++ to_string(x)],
     %{a: x, b: x * x, tags: ['p', 'q', x], label: "lab" ++ to_string(x)}}
}

# The free-ing C-level consumer: a process that repeatedly sleep()s. Each
# sleep() is sw_receive_any(...)+free(msg). If a VALUE message arrives during a
# sleep while a pmap on this thread leaked adopt=1, the free() hits an
# arena-interior pointer.
fun sleeper(rounds) {
    if (rounds <= 0) { 'ok' }
    else {
        sleep(30)            # parks in sw_receive_any; frees whatever it pops
        sleeper(rounds - 1)
    }
}

# pmap worker: yield so the driver must BLOCK in sw_recv_any_adopt (the only way
# to context-switch mid-pmap and leak adopt=1 to the sleeper).
fun pm_body(x) {
    sleep(10)
    fat(x)
}

fun driver(coord, base) {
    lst = [base + 1, base + 2, base + 3, base + 4, base + 5, base + 6]
    res = pmap(fun(x) { pm_body(x) }, lst)
    sleep(20)
    fails = verify(res, 0)
    send(coord, {'done', fails})
}

fun verify(res, acc) {
    if (length(res) == 0) { acc }
    else {
        r = hd(res)
        x = elem(r, 1)
        f0 = assert_eq("tag", elem(r, 0), 'payload')
        f1 = assert_eq("list", elem(r, 2), [x, x * 2, x * 3, "s" ++ to_string(x)])
        f2 = assert_eq("b", map_get(elem(r, 3), 'b'), x * x)
        f3 = assert_eq("tags", map_get(elem(r, 3), 'tags'), ['p', 'q', x])
        verify(tl(res), acc + f0 + f1 + f2 + f3)
    }
}

# The poker: hammer the sleeper with fat VALUE messages so one lands during a
# sleep() while a driver's adopt=1 is live on the shared thread.
fun poker(target, n) {
    if (n <= 0) { 'ok' }
    else {
        send(target, fat(n))
        sleep(5)
        poker(target, n - 1)
    }
}

fun spawn_drivers(coord, n) {
    if (n <= 0) { 'ok' }
    else {
        spawn(driver(coord, n * 100))
        spawn_drivers(coord, n - 1)
    }
}

fun collect(count, acc) {
    if (count <= 0) { acc }
    else {
        m = receive {
            {'done', fails} -> fails
            after 30000 { -1 }
        }
        if (m < 0) { print("FAIL collect: timeout") ; acc + 1 }
        else { collect(count - 1, acc + m) }
    }
}

fun job_count(args) { if (length(args) >= 2) { to_int(hd(tl(args))) } else { 10 } }

fun main() {
    n = job_count(os_args())
    print("tls_repro drivers=" ++ to_string(n))
    # Long-lived sleeper that frees whatever it pops in sw_receive_any.
    s = spawn(sleeper(400))
    # Pokers that flood the sleeper with fat VALUE messages.
    spawn(poker(s, 120))
    spawn(poker(s, 120))
    # Drivers running pmap concurrently -> leak adopt=1 across the shared thread.
    spawn_drivers(self(), n)
    fails = collect(n, 0)
    if (fails == 0) { print("OK tls_repro " ++ to_string(n) ++ " drivers clean") ; sys_exit(0) }
    else { print("FAIL tls_repro " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
