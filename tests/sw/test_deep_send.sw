module Test_deep_send

# Regression: a value nested deeper than 256 levels must never arrive
# truncated. The local copy on send used to return nil past depth 256, so a
# 300-deep list reached the receiver as a 256-deep one with no error. The copy
# is now iterative with no depth cap, so every depth arrives intact.

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

# 200 levels is within the old cap: it must arrive intact.
fun test_within_cap() {
    me = self()
    r = spawn(receiver(me))
    send(r, {'deep', nest('leaf', 200)})
    receive {
        {'depth', d} -> assert_eq("deep_within_cap", d, 200)
        after 5000 { assert_eq("deep_within_cap", "TIMEOUT", "MATCH") }
    }
}

# 300 levels is past the old cap: it must still arrive intact, not truncated.
fun test_past_cap() {
    me = self()
    r = spawn(receiver(me))
    send(r, {'deep', nest('leaf', 300)})
    receive {
        {'depth', d} -> assert_eq("deep_past_cap_intact", d, 300)
        after 5000 { assert_eq("deep_past_cap_intact", "TIMEOUT", "MATCH") }
    }
}

# A deep value stored in ETS and then overwritten: the global-heap free of the
# old copy must walk it without overflowing the fiber stack.
fun test_ets_replace_deep() {
    t = ets_new()
    ets_put(t, 'k', nest('leaf', 5000))
    ets_put(t, 'k', 1)
    assert_eq("ets_replace_deep", ets_get(t, 'k'), 1)
}

fun main() {
    fails = test_within_cap() + test_past_cap() + test_ets_replace_deep()
    if (fails == 0) {
        print("OK test_deep_send 3/3")
        sys_exit(0)
    } else {
        print("FAIL test_deep_send")
        sys_exit(1)
    }
}
main()
