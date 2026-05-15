module Test_case_and_format

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# ---- case ----

fun describe(x) {
    case x {
        0 -> "zero"
        1 -> "one"
        n when n > 0 -> "positive"
        _ -> "negative"
    }
}

fun test_case_literal_match() { assert_eq("case_literal", describe(1), "one") }
fun test_case_guard_pass() { assert_eq("case_guard_pass", describe(7), "positive") }
fun test_case_guard_fall() {
    # n when n>0 fails for -3, must fall through to _ catchall
    assert_eq("case_guard_fall_through", describe(-3), "negative")
}

fun classify(msg) {
    case msg {
        {'ok', v}      -> "ok:" ++ to_string(v)
        {'error', why} -> "err:" ++ to_string(why)
        'done'         -> "done"
        other          -> "other:" ++ to_string(other)
    }
}

fun test_case_tuple_bind() { assert_eq("case_tuple_bind", classify({'ok', 42}), "ok:42") }
fun test_case_tuple_alt() { assert_eq("case_tuple_alt", classify({'error', "boom"}), "err:boom") }
fun test_case_atom_match() { assert_eq("case_atom_match", classify('done'), "done") }
fun test_case_catchall() {
    # The catchall renders the input via to_string — since we improved
    # to_string for tuples, this exercises both paths.
    assert_eq("case_catchall", classify({'wat', 1}), "other:{:wat, 1}")
}

# ---- format ----

fun test_format_basic() { assert_eq("fmt_basic", format("hi {}", "world"), "hi world") }
fun test_format_multi() { assert_eq("fmt_multi", format("{}+{}={}", 2, 3, 5), "2+3=5") }
fun test_format_int() { assert_eq("fmt_int", format("n={}", 42), "n=42") }
fun test_format_tuple() { assert_eq("fmt_tuple", format("t={}", {'ok', 1}), "t={:ok, 1}") }
fun test_format_list() { assert_eq("fmt_list", format("l={}", [1, 2, 3]), "l=[1, 2, 3]") }
fun test_format_escape() { assert_eq("fmt_escape", format("{{ + }} = {}", 1), "{ + } = 1") }
fun test_format_missing() { assert_eq("fmt_missing", format("a={} b={}", 9), "a=9 b={}") }

# ---- new builtins ----

fun test_map_size() {
    m = map_put(map_put(map_put(%{}, 'a', 1), 'b', 2), 'c', 3)
    assert_eq("map_size", map_size(m), 3)
}

fun test_map_remove() {
    m = map_put(map_put(%{}, 'a', 1), 'b', 2)
    pruned = map_remove(m, 'a')
    assert_eq("map_remove", map_size(pruned), 1)
}

fun test_length_on_map() {
    m = map_put(%{}, 'k', 'v')
    assert_eq("length_on_map", length(m), 1)
}

fun main() {
    fails = 0
    fails = fails + test_case_literal_match()
    fails = fails + test_case_guard_pass()
    fails = fails + test_case_guard_fall()
    fails = fails + test_case_tuple_bind()
    fails = fails + test_case_tuple_alt()
    fails = fails + test_case_atom_match()
    fails = fails + test_case_catchall()
    fails = fails + test_format_basic()
    fails = fails + test_format_multi()
    fails = fails + test_format_int()
    fails = fails + test_format_tuple()
    fails = fails + test_format_list()
    fails = fails + test_format_escape()
    fails = fails + test_format_missing()
    fails = fails + test_map_size()
    fails = fails + test_map_remove()
    fails = fails + test_length_on_map()
    if (fails == 0) { print("OK case_and_format 17/17") ; sys_exit(0) }
    else { print("FAIL case_and_format " ++ to_string(fails)) ; sys_exit(1) }
}
