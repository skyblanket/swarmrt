module Test_monitor_pid

# Regression: a pid delivered inside {'DOWN', ref, kind, PID, reason} and
# {'EXIT', PID, reason} must compare == the pid spawn() returned. Before
# the fix the DOWN/EXIT pid arrived as a bare int (sw_val_int), so the
# idiomatic supervisor pattern `dpid == child_pid` was silently always
# false — only `ref` ever compared. Also covers spawn_monitor(fn) which
# atomically spawns + monitors and returns {pid, ref}.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun child() {
    receive { 'die' -> 'ok' }
}

fun boomer() {
    panic("boom")
}

# spawn + monitor: the DOWN pid must == the pid spawn() handed back.
fun test_down_pid_identity() {
    cpid = spawn(child())
    ref = monitor(cpid)
    send(cpid, 'die')
    receive {
        {'DOWN', _, _, dpid, _} -> assert_eq("down_pid_identity", dpid == cpid, true)
        after 2000 { assert_eq("down_pid_identity", "TIMEOUT", "MATCH") }
    }
}

# spawn_monitor returns {pid, ref}; DOWN delivers BOTH the matching pid
# and the matching ref.
fun test_spawn_monitor_pid_and_ref() {
    res = spawn_monitor(child())
    cpid = elem(res, 0)
    ref = elem(res, 1)
    send(cpid, 'die')
    receive {
        {'DOWN', dref, _, dpid, _} ->
            assert_eq("spawn_monitor_pid", dpid == cpid, true) +
            assert_eq("spawn_monitor_ref", dref == ref, true)
        after 2000 { assert_eq("spawn_monitor", "TIMEOUT", "MATCH") }
    }
}

# trap_exit: {'EXIT', from, reason} — `from` must == the linked pid.
fun test_exit_pid_identity() {
    trap_exit('true')
    cpid = spawn(boomer())
    link(cpid)
    receive {
        {'EXIT', from, _} -> assert_eq("exit_pid_identity", from == cpid, true)
        after 2000 { assert_eq("exit_pid_identity", "TIMEOUT", "MATCH") }
    }
}

fun main() {
    fails = 0
    fails = fails + test_down_pid_identity()
    fails = fails + test_spawn_monitor_pid_and_ref()
    fails = fails + test_exit_pid_identity()
    if (fails == 0) { print("OK monitor_pid 4/4") ; sys_exit(0) }
    else { print("FAIL monitor_pid " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
