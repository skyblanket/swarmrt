module Test_dynsup

# Runtime dynamic child supervision (DynamicSupervisor / start_child).
# Compiled-only: the tree-walking interpreter has no scheduler, so the
# process/supervisor builtins degrade to nil there — this file is run by
# the compile-then-execute path in run_tests.sh, like test_processes.sw.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# A child that answers pings and keeps looping (stays alive + supervised).
fun pinger() {
    receive {
        {'ping', from} -> send(from, 'pong') ; pinger()
        'die' -> 'ok'
    }
}

# A permanent child that crashes on demand — used to prove restart.
fun crasher(reg) {
    receive {
        'boom' -> panic("boom")
        {'ping', from} -> send(from, 'pong') ; crasher(reg)
    }
}

# A child that crashes immediately on start — used to trip the circuit breaker
# (it self-crashes on every (re)start, so the supervisor exhausts its restart
# budget on its own with no driving from the test).
fun cb_crasher() {
    panic("cb boom")
}

# Happy path: empty supervisor, add a child at runtime, it works, count tracks it.
fun test_start_child_and_count() {
    sup = dyn_supervisor()
    fails = assert_eq("empty_count", sup_count_children(sup), 0)

    child = sup_start_child(sup, {'w1', fun() { pinger() }, 'permanent'})
    fails = fails + assert_eq("child_is_pid", child == nil, false)
    fails = fails + assert_eq("count_after_add", sup_count_children(sup), 1)

    # The supervised child is a normal process: round-trip a message.
    send(child, {'ping', self()})
    reply = receive {
        'pong' -> 'pong'
        after 2000 { 'timeout' }
    }
    fails = fails + assert_eq("child_responds", reply, 'pong')
    fails
}

# Add several children, terminate one, count reflects the change.
fun test_terminate_child() {
    sup = dyn_supervisor()
    a = sup_start_child(sup, {nil, fun() { pinger() }, 'permanent'})
    b = sup_start_child(sup, {nil, fun() { pinger() }, 'permanent'})
    fails = assert_eq("count_two", sup_count_children(sup), 2)

    ok = sup_terminate_child(sup, a)
    fails = fails + assert_eq("terminate_ok", ok, 'ok')
    fails = fails + assert_eq("count_after_term", sup_count_children(sup), 1)

    # b is still alive and supervised.
    send(b, {'ping', self()})
    reply = receive {
        'pong' -> 'pong'
        after 2000 { 'timeout' }
    }
    fails = fails + assert_eq("survivor_alive", reply, 'pong')
    fails
}

# Failure/restart path: a :permanent child that crashes is restarted under its
# registered name, so the name still resolves to a live process and the
# supervisor's child count is unchanged.
fun test_permanent_child_restarts() {
    sup = dyn_supervisor()
    c = sup_start_child(sup, {'crash_worker', fun() { crasher('crash_worker') }, 'permanent'})
    fails = assert_eq("count_one", sup_count_children(sup), 1)

    # Crash it. The supervisor traps the DOWN and restarts under the same name.
    send(c, 'boom')
    sleep(300)

    revived = whereis('crash_worker')
    fails = fails + assert_eq("revived_nonnil", revived == nil, false)
    fails = fails + assert_eq("count_unchanged", sup_count_children(sup), 1)

    send(revived, {'ping', self()})
    reply = receive {
        'pong' -> 'pong'
        after 2000 { 'timeout' }
    }
    fails = fails + assert_eq("revived_responds", reply, 'pong')
    fails
}

# Edge: bad inputs must not crash — they return nil / 'error' / 0.
fun test_bad_inputs_degrade() {
    sup = dyn_supervisor()
    # Missing fn in the child tuple -> nil, count stays 0.
    bad = sup_start_child(sup, {'oops'})
    fails = assert_eq("bad_spec_nil", bad, nil)
    fails = fails + assert_eq("count_still_zero", sup_count_children(sup), 0)
    # Terminating something that isn't a child of this sup -> 'error'.
    other = dyn_supervisor()
    fails = fails + assert_eq("term_foreign_error", sup_terminate_child(sup, other), 'error')
    fails
}

# Circuit breaker: a :permanent child stuck in a crash loop must NOT restart
# forever. After max_restarts within max_seconds, the supervisor kills its
# children and shuts itself down. We monitor the supervisor and assert it dies
# (its DOWN carries the supervisor's own pid). This is the behavior an agent
# author is most surprised by ("why did my supervisor vanish?") — so it's the
# one we most want pinned.
fun test_circuit_breaker_shutdown() {
    # max_restarts=2 in a wide 60s window: the self-crashing child crashes 3x
    # (initial start + 2 restarts); the 3rd DOWN trips the breaker.
    sup = dyn_supervisor(2, 60)
    ref = monitor(sup)
    sup_start_child(sup, {'cb_worker', fun() { cb_crasher() }, 'permanent'})

    # The crash -> restart -> crash cascade trips the breaker within ms; the
    # supervisor process then exits, firing our monitor's DOWN with sup's pid.
    died = receive {
        {'DOWN', _, _, dpid, _} -> dpid == sup
        after 3000 { 'no_down' }
    }
    assert_eq("breaker_sup_died", died, true)
}

fun main() {
    fails = 0
    fails = fails + test_start_child_and_count()
    fails = fails + test_terminate_child()
    fails = fails + test_permanent_child_restarts()
    fails = fails + test_bad_inputs_degrade()
    fails = fails + test_circuit_breaker_shutdown()
    if (fails == 0) { print("OK dynsup 16/16") ; sys_exit(0) }
    else { print("FAIL dynsup " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
