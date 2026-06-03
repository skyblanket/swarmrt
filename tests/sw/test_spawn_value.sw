# test_spawn_value.sw — spawn / spawn_monitor must accept a function VALUE,
# not just `spawn(named_call(...))` or the inline `spawn(fn(){...})` literal.
# The first-try-writeability gap this closes: `f = fn(){...} ; spawn(f)`
# (a closure-valued local) used to be a hard codegen error —
#   "spawn(...) requires a function call ... or a lambda"
# — even though it is the form LLMs reach for (task_stream had to stash the
# closure in an ETS '__fn' slot to work around it). Now any non-call inner
# (local/global closure value, inline lambda, or a function by-name) is
# evaluated to a value and run via the generic lambda-spawn trampoline.
#
# Observation is via a shared ETS table the spawned process writes; main
# sleeps to let the child run, then reads it back. Compiled twin of
# tests/sw/run/test_spawn_value.sw — the two MUST agree (modulo the
# spawn_monitor {pid,ref} shape, which the interpreter has no real scheduler
# for and so the run/ twin asserts only the side effect).
#
# Self-checking: prints "OK spawn_value N/N" + sys_exit(0) on full pass.

module Test_spawn_value

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# 1. spawn a closure-valued LOCAL (the headline gap).
fun test_spawn_local_closure() {
    t = ets_new()
    f = fn() { ets_put(t, 'k', 'local_ran') }
    spawn(f)
    sleep(150)
    assert_eq("spawn_local_closure", ets_get(t, 'k'), 'local_ran')
}

# 2. spawn an INLINE lambda (must keep working — prior phase).
fun test_spawn_inline() {
    t = ets_new()
    spawn(fn() { ets_put(t, 'k', 'inline_ran') })
    sleep(150)
    assert_eq("spawn_inline", ets_get(t, 'k'), 'inline_ran')
}

# 3. a closure that CAPTURES a local must run with its capture intact.
fun test_spawn_capture() {
    t = ets_new()
    tag = 'captured_ok'
    f = fn() { ets_put(t, 'k', tag) }
    spawn(f)
    sleep(150)
    assert_eq("spawn_capture", ets_get(t, 'k'), 'captured_ok')
}

# 4. spawn_monitor of a closure-valued local: returns {pid, ref} AND runs it.
#    The {pid, ref} shape (0-indexed): elem(pm, 0) is the pid, elem(pm, 1) is
#    the monitor ref (an int). We assert both the side effect and that the ref
#    element is a non-negative int — proving spawn_monitor produced its
#    2-tuple, not a bare pid.
fun test_spawn_monitor_local() {
    t = ets_new()
    f = fn() { ets_put(t, 'k', 'monitored_ran') }
    pm = spawn_monitor(f)
    sleep(150)
    ref = elem(pm, 1)
    if (ets_get(t, 'k') == 'monitored_ran' && ref >= 0) {
        print("PASS spawn_monitor_local") ; 0
    } else {
        print("FAIL spawn_monitor_local: side=" ++ to_string(ets_get(t, 'k')) ++
              " ref=" ++ to_string(ref))
        1
    }
}

fun main() {
    fails = 0
    fails = fails + test_spawn_local_closure()
    fails = fails + test_spawn_inline()
    fails = fails + test_spawn_capture()
    fails = fails + test_spawn_monitor_local()
    if (fails == 0) { print("OK spawn_value 4/4") ; sys_exit(0) }
    else { print("FAIL spawn_value " ++ to_string(fails)) ; sys_exit(1) }
}
