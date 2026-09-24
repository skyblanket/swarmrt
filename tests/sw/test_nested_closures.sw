module Test_nested_closures

# Regression: a lambda nested inside another lambda must capture variables
# from every enclosing scope — including the common "fan out, reply to me"
# shape `each(xs, fun(x) { spawn(fun() { send(me, ...) }) })`, where `me`
# lives two scopes up. That used to fail to compile ('me' undeclared).

import Std

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun gather(n, acc) {
    if (n == 0) { acc }
    else {
        receive {
            {'sq', v} -> gather(n - 1, acc + v)
            after 5000 -> acc
        }
    }
}

fun test_spawn_in_nested() {
    me = self()
    scale = 10
    Std.each([1, 2, 3], fun(x) { spawn(fun() { send(me, {'sq', x * x * scale}) }) })
    assert_eq("spawn_in_nested", gather(3, 0), 140)
}

fun test_three_levels() {
    a = 1
    f = fun() { b = 2 ; fun() { fun() { a + b + 3 } } }
    g = f()
    h = g()
    assert_eq("three_levels", h(), 6)
}

fun test_receive_in_lambda() {
    me = self()
    want = 'ping'
    worker = fun() { receive { m -> send(me, {'echo', m, want}) after 2000 -> 0 } }
    pid = spawn(worker())
    send(pid, 'hello')
    got = receive { {'echo', m, w} -> {m, w} after 2000 -> 'timeout' }
    assert_eq("receive_in_lambda", got, {'hello', 'ping'})
}

fun main() {
    fails = test_spawn_in_nested() + test_three_levels() + test_receive_in_lambda()
    if (fails == 0) { print("OK nested_closures 3/3") ; sys_exit(0) }
    else { print("FAIL nested_closures") ; sys_exit(1) }
}
