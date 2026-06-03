# test_spawn_value.sw (interp twin) — `spawn(f)` / `spawn_monitor(f)` of a
# function VALUE (closure-valued local, inline lambda) must RUN the closure
# body in the tree-walking interpreter too, not silently drop it.
#
# Background: the interpreter has no real scheduler, so spawn runs the work
# synchronously and returns a nil pid (the compiled path is the concurrent
# one — asserted in tests/sw/test_spawn_value.sw). The parity gap this guards:
# `spawn(f)` where `f` is a fun VALUE used to evaluate the ident to the fun
# and then DISCARD it — the body never ran, while the compiled twin ran it.
# Now the interpreter applies a fun value reached as a bare (non-call) inner.
#
# We observe via a shared ETS table the spawned closure writes, then read it
# back (synchronous in interp, so no sleep needed for correctness — a short
# sleep is harmless and keeps the shape identical to the compiled twin).
# Self-checking: prints "SPAWN_VALUE_INTERP_OK" + sys_exit(0).

module Test_spawn_value_interp

fun chk(name, actual, expected) {
    if (actual == expected) { 0 }
    else {
        print("SPAWN_VALUE_INTERP_FAIL " ++ name ++ ": expected " ++
              to_string(expected) ++ ", got " ++ to_string(actual))
        1
    }
}

fun main() {
    fails = 0

    # spawn a closure-valued local
    t1 = ets_new()
    f1 = fn() { ets_put(t1, 'k', 'local_ran') }
    spawn(f1)
    sleep(50)
    fails = fails + chk("spawn_local_closure", ets_get(t1, 'k'), 'local_ran')

    # spawn an inline lambda
    t2 = ets_new()
    spawn(fn() { ets_put(t2, 'k', 'inline_ran') })
    sleep(50)
    fails = fails + chk("spawn_inline", ets_get(t2, 'k'), 'inline_ran')

    # closure capturing a local
    t3 = ets_new()
    tag = 'captured_ok'
    f3 = fn() { ets_put(t3, 'k', tag) }
    spawn(f3)
    sleep(50)
    fails = fails + chk("spawn_capture", ets_get(t3, 'k'), 'captured_ok')

    # spawn_monitor of a closure-valued local runs the body too
    t4 = ets_new()
    f4 = fn() { ets_put(t4, 'k', 'monitored_ran') }
    spawn_monitor(f4)
    sleep(50)
    fails = fails + chk("spawn_monitor_local", ets_get(t4, 'k'), 'monitored_ran')

    if (fails == 0) { print("SPAWN_VALUE_INTERP_OK") ; sys_exit(0) }
    else { sys_exit(1) }
}
