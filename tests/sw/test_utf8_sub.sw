module Test_utf8_sub

import Prompt

# #170: string_sub / string_truncate count bytes (string_length is bytes) but
# must never cut a UTF-8 character: the end moves back to the start of a
# character it would split, the start moves forward past continuation bytes.
# Both backends (compiled here + the tests/sw/run/ copy via swc run).
# "—" is 3 bytes (E2 80 94); "é" is 2 (C3 A9); "😀" is 4 (F0 9F 98 80).

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ base64_encode(to_string(expected)) ++
              ", got " ++ base64_encode(to_string(actual)))
        1
    }
}

fun test_sub() {
    s = "ab—cd"
    fails = 0
    # End lands inside "—": moves back to its start.
    fails = fails + assert_eq("sub_end_in_char", string_sub(s, 0, 3), "ab")
    fails = fails + assert_eq("sub_end_in_char2", string_sub(s, 0, 4), "ab")
    fails = fails + assert_eq("sub_end_after_char", string_sub(s, 0, 5), "ab—")
    # Start lands inside "—": moves forward past it.
    fails = fails + assert_eq("sub_start_in_char", string_sub(s, 3, 4), "cd")
    fails = fails + assert_eq("sub_start_in_char2", string_sub(s, 4, 3), "cd")
    fails = fails + assert_eq("sub_start_on_char", string_sub(s, 2, 3), "—")
    # Both edges in a character, and a slice entirely inside one.
    fails = fails + assert_eq("sub_both_edges", string_sub("é—é", 1, 5), "—")
    fails = fails + assert_eq("sub_inside_char", string_sub(s, 3, 1), "")
    fails = fails + assert_eq("sub_lead_byte_only", string_sub(s, 2, 1), "")
    # 4-byte character at each edge.
    e = "😀x😀"
    fails = fails + assert_eq("sub_4byte_end", string_sub(e, 0, 7), "😀x")
    fails = fails + assert_eq("sub_4byte_start", string_sub(e, 1, 8), "x😀")
    # ASCII and whole-string behaviour unchanged.
    fails = fails + assert_eq("sub_ascii", string_sub("hello", 1, 3), "ell")
    fails = fails + assert_eq("sub_to_end", string_sub(s, 2, 100), "—cd")
    fails = fails + assert_eq("sub_past_end", string_sub(s, 99, 1), "")
    fails = fails + assert_eq("sub_neg_start", string_sub(s, -5, 2), "ab")
    fails
}

fun test_truncate() {
    s = "ab—cd"
    fails = 0
    fails = fails + assert_eq("trunc_in_char", string_truncate(s, 4), "ab")
    fails = fails + assert_eq("trunc_in_char1", string_truncate(s, 3), "ab")
    fails = fails + assert_eq("trunc_after_char", string_truncate(s, 5), "ab—")
    fails = fails + assert_eq("trunc_whole", string_truncate(s, 7), s)
    fails = fails + assert_eq("trunc_4byte", string_truncate("😀", 3), "")
    fails = fails + assert_eq("trunc_ascii", string_truncate("hello", 2), "he")
    fails = fails + assert_eq("trunc_zero", string_truncate(s, 0), "")
    fails
}

# Keep the last n bytes, as the issue's log-tail case does.
fun tail(raw, n) { string_sub(raw, string_length(raw) - n, n) }

fun test_callers() {
    fails = 0
    log = "gate failed — retry later"
    fails = fails + assert_eq("tail_mid_char", tail(log, 14), " retry later")
    # Prompt.render used to copy byte by byte with string_sub(t, i, 1).
    fails = fails + assert_eq("prompt_utf8",
        Prompt.render("héllo — {{name}} 😀!", %{"name" => "Zoë"}), "héllo — Zoë 😀!")
    fails = fails + assert_eq("prompt_no_vars", Prompt.render("naïve", %{}), "naïve")
    fails = fails + assert_eq("prompt_braces", Prompt.render("{a} {{x}}{{y}}é", %{"x" => 1, "y" => "—"}), "{a} 1—é")
    fails
}

fun main() {
    fails = test_sub() + test_truncate() + test_callers()
    if (fails == 0) { print("OK utf8_sub") ; sys_exit(0) }
    else { print("FAIL utf8_sub") ; sys_exit(1) }
}
