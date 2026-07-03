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
# back. `swc run` now boots a real single-scheduler runtime (spawn is
# scheduled, not synchronous), so — like the compiled twin — we POLL the ETS
# key until the child has run rather than relying on a fixed sleep (a fixed
# sleep(50) here was a brittle timing flake that went red on a contended CI
# runner where 50ms wasn't enough for the child to be scheduled).
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

# Poll the ETS key until the spawned child wrote it, up to `tries` 20ms ticks
# (~1s budget). Returns as soon as it appears — fast when idle, tolerant of a
# slow/loaded runner.
fun wait_for(t, key, tries) {
    v = ets_get(t, key)
    if (v != nil) { v }
    else { if (tries <= 0) { nil } else { sleep(20) ; wait_for(t, key, tries - 1) } }
}

fun main() {
    fails = 0

    # spawn a closure-valued local
    t1 = ets_new()
    f1 = fn() { ets_put(t1, 'k', 'local_ran') }
    spawn(f1)
    fails = fails + chk("spawn_local_closure", wait_for(t1, 'k', 50), 'local_ran')

    # spawn an inline lambda
    t2 = ets_new()
    spawn(fn() { ets_put(t2, 'k', 'inline_ran') })
    fails = fails + chk("spawn_inline", wait_for(t2, 'k', 50), 'inline_ran')

    # closure capturing a local
    t3 = ets_new()
    tag = 'captured_ok'
    f3 = fn() { ets_put(t3, 'k', tag) }
    spawn(f3)
    fails = fails + chk("spawn_capture", wait_for(t3, 'k', 50), 'captured_ok')

    # spawn_monitor of a closure-valued local runs the body too
    t4 = ets_new()
    f4 = fn() { ets_put(t4, 'k', 'monitored_ran') }
    spawn_monitor(f4)
    fails = fails + chk("spawn_monitor_local", wait_for(t4, 'k', 50), 'monitored_ran')

    if (fails == 0) { print("SPAWN_VALUE_INTERP_OK") ; sys_exit(0) }
    else { sys_exit(1) }
}
