module Test_strings

fun check_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun test_index_of_hit() { check_eq("index_of_hit", string_index_of("hello world", "world"), 6) }
fun test_index_of_miss() { check_eq("index_of_miss", string_index_of("hello", "xyz"), -1) }
fun test_index_of_empty() { check_eq("index_of_empty", string_index_of("hello", ""), 0) }

fun test_split() {
    parts = string_split("a,b,c", ",")
    check_eq("split_count", length(parts), 3)
}

fun test_concat() { check_eq("concat", "foo" ++ "bar", "foobar") }

fun test_base64_roundtrip() {
    src = "the quick brown fox jumps over the lazy dog"
    enc = base64_encode(src)
    dec = base64_decode(enc)
    check_eq("base64_roundtrip", dec, src)
}

fun test_base64_known() {
    # `hello` → `aGVsbG8=` per RFC 4648
    check_eq("base64_known", base64_encode("hello"), "aGVsbG8=")
}

# f-string `{{`/`}}` escape to literal braces (so source full of `{ }` — e.g. an
# agent composing tool source — survives an f-string); single `{ }` still interpolate.
fun test_fstring_escape() {
    x = 5
    fails = check_eq("fstring_escape_literal", f"a {{b}} c", "a {b} c")
    fails = fails + check_eq("fstring_interp_still_works", f"v={x}", "v=5")
    fails = fails + check_eq("fstring_mixed", f"{{ {x} }}", "{ 5 }")
    fails
}

fun main() {
    fails = 0
    fails = fails + test_index_of_hit()
    fails = fails + test_index_of_miss()
    fails = fails + test_index_of_empty()
    fails = fails + test_split()
    fails = fails + test_concat()
    fails = fails + test_base64_roundtrip()
    fails = fails + test_base64_known()
    fails = fails + test_fstring_escape()
    if (fails == 0) { print("OK strings 10/10") ; sys_exit(0) }
    else { print("FAIL strings " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
