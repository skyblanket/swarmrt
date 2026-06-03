module Test_processes

# Bare-minimum spawn/send/receive coverage.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun echo_loop(parent) {
    receive {
        {'ping', n} -> send(parent, {'pong', n + 1}) ; echo_loop(parent)
        'stop' -> 'ok'
    }
}

fun test_send_receive() {
    pid = spawn(echo_loop(self()))
    send(pid, {'ping', 41})
    n = receive { {'pong', v} -> v }
    send(pid, 'stop')
    assert_eq("send_receive", n, 42)
}

fun test_self_pid_nonnil() {
    me = self()
    if (me == nil) { print("FAIL self_pid_nonnil") ; 1 }
    else { print("PASS self_pid_nonnil") ; 0 }
}

# ETS basics — used by every long-lived agent state in swarm-code.
fun test_ets_put_get() {
    t = ets_new()
    ets_put(t, 'k', 7)
    assert_eq("ets_put_get", ets_get(t, 'k'), 7)
}

fun test_ets_missing_returns_nil() {
    t = ets_new()
    assert_eq("ets_missing", ets_get(t, 'nope'), nil)
}

# Regression: ets_list must return a list of {k, v} tuples (per
# docs/SW_LANGUAGE.md), NOT keys-only and NOT table IDs. Compiled and
# interpreter paths must agree. A single-key table makes the expected
# shape order-independent.
fun test_ets_list_returns_kv_pairs() {
    t = ets_new()
    ets_put(t, 'k', 9)
    assert_eq("ets_list_kv", ets_list(t), [{'k', 9}])
}

fun main() {
    fails = 0
    fails = fails + test_send_receive()
    fails = fails + test_self_pid_nonnil()
    fails = fails + test_ets_put_get()
    fails = fails + test_ets_missing_returns_nil()
    fails = fails + test_ets_list_returns_kv_pairs()
    if (fails == 0) { print("OK processes 5/5") ; sys_exit(0) }
    else { print("FAIL processes " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
