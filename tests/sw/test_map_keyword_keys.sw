module Test_map_keyword_keys

# Regression: a map-literal key that is also a reserved word (`spawn`,
# `receive`, `self`, ...) must parse as a `name:` key. It used to fall
# through to expression parsing, so `%{spawn: 1}` failed with
# "expected '(', got ':'" because `spawn` was read as the spawn() builtin.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun test_keyword_literal_keys() {
    m = %{spawn: 1, receive: 2, send: 3, self: 4, fun: 5, ok: 6}
    fails = 0
    fails = fails + assert_eq("spawn_key", map_get(m, 'spawn'), 1)
    fails = fails + assert_eq("receive_key", map_get(m, 'receive'), 2)
    fails = fails + assert_eq("send_key", map_get(m, 'send'), 3)
    fails = fails + assert_eq("self_key", map_get(m, 'self'), 4)
    fails = fails + assert_eq("fun_key", map_get(m, 'fun'), 5)
    fails = fails + assert_eq("plain_key", map_get(m, 'ok'), 6)
    fails
}

fun test_keyword_key_pattern() {
    m = %{spawn: 7}
    got = case m {
        %{spawn: x} -> x
        _ -> 0
    }
    assert_eq("spawn_key_pattern", got, 7)
}

fun main() {
    fails = 0
    fails = fails + test_keyword_literal_keys()
    fails = fails + test_keyword_key_pattern()
    if (fails == 0) { print("OK map_keyword_keys 7/7") ; sys_exit(0) }
    else { print("FAIL map_keyword_keys " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
