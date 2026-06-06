module Test_json

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun assert_true(name, cond) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name) ; 1 }
}

fun test_string_roundtrip() {
    enc = json_encode("hello")
    dec = json_decode(enc)
    assert_eq("string_roundtrip", dec, "hello")
}

fun test_int_roundtrip() {
    enc = json_encode(42)
    dec = json_decode(enc)
    assert_eq("int_roundtrip", dec, 42)
}

fun test_map_roundtrip() {
    m = %{a: 1, b: "two"}
    enc = json_encode(m)
    dec = json_decode(enc)
    assert_eq("map_roundtrip_a", map_get(dec, 'a'), 1)
}

fun test_unicode_escape() {
    # `&` (ampersand) must decode to literal `&`, not the
    # 6-char string "u0026". This was the 2026-05-11 fix.
    raw = "\"\\u0026\\u0026\""
    dec = json_decode(raw)
    assert_eq("unicode_escape", dec, "&&")
}

fun test_nested_list_roundtrip() {
    src = [1, 2, 3, 4]
    enc = json_encode(src)
    dec = json_decode(enc)
    assert_eq("nested_list_len", length(dec), 4)
}

# Deeply-nested JSON must NOT crash (depth guard): over the cap it returns a
# truncated/partial value, and a normal decode still works right after. If the
# decoder SIGILL'd on the deep input, the program never reaches the assertion.
fun rep(s, n, acc) { if (n <= 0) { acc } else { rep(s, n - 1, acc ++ s) } }
fun test_deep_json_no_crash() {
    deep = rep("[", 1000, "") ++ rep("]", 1000, "")
    json_decode(deep)                                   # must not crash
    healthy = json_decode("[1, 2, 3]")                  # decoder still works after
    assert_eq("deep_json_survived_then_works", length(healthy), 3)
}

fun main() {
    fails = 0
    fails = fails + test_string_roundtrip()
    fails = fails + test_int_roundtrip()
    fails = fails + test_map_roundtrip()
    fails = fails + test_unicode_escape()
    fails = fails + test_nested_list_roundtrip()
    fails = fails + test_deep_json_no_crash()
    if (fails == 0) { print("OK json 6/6") ; sys_exit(0) }
    else { print("FAIL json " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
