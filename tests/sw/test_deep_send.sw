module Test_deep_send

# Regression: a value nested deeper than 256 levels must never arrive
# truncated. The copy on send used to return nil past depth 256, so a
# 300-deep list reached the receiver as a 256-deep one with no error. Now a
# value that cannot be copied faithfully panics the sending process: the
# receiver never sees a 'deep' message, and the sender's DOWN arrives.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun nest(acc, n) {
    if (n <= 0) { acc }
    else { nest([acc], n - 1) }
}

fun depth(v) {
    case v {
        [x] -> 1 + depth(x)
        _   -> 0
    }
}

fun receiver(parent) {
    v = receive { {'deep', x} -> x after 5000 { 'timeout' } }
    send(parent, {'depth', depth(v)})
    0
}

fun sender(parent, n) {
    send(parent, {'deep', nest('leaf', n)})
    0
}

# 200 levels is within the cap: it must arrive intact.
fun test_within_cap() {
    me = self()
    r = spawn(receiver(me))
    send(r, {'deep', nest('leaf', 200)})
    receive {
        {'depth', d} -> assert_eq("deep_within_cap", d, 200)
        after 5000 { assert_eq("deep_within_cap", "TIMEOUT", "MATCH") }
    }
}

# 300 levels is past the cap: the sender panics, so the receiver gets no
# truncated copy — only the sender's DOWN.
fun test_past_cap_panics() {
    me = self()
    res = spawn_monitor(sender(me, 300))
    ref = elem(res, 1)
    receive {
        {'deep', _} -> assert_eq("deep_past_cap_no_truncated_copy", "DELIVERED", "NONE")
        {'DOWN', dref, _, _, _} -> assert_eq("deep_past_cap_panics", dref == ref, true)
        after 5000 { assert_eq("deep_past_cap_panics", "TIMEOUT", "MATCH") }
    }
}

fun main() {
    fails = test_within_cap() + test_past_cap_panics()
    if (fails == 0) {
        print("OK test_deep_send 2/2")
        sys_exit(0)
    } else {
        print("FAIL test_deep_send")
        sys_exit(1)
    }
}
main()
