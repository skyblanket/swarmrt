module Test_conversions

# to_int / to_float (parse strings, coerce numbers, nil on non-numeric) + uuid +
# now_iso. Shared C helpers, so this runs identically in BOTH backends (compiled
# here + tests/sw/run/ copy via swc run).

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun test_to_int() {
    fails = 0
    fails = fails + assert_eq("to_int_str", to_int("42"), 42)
    fails = fails + assert_eq("to_int_neg", to_int("-7"), -7)
    fails = fails + assert_eq("to_int_float", to_int(3.9), 3)
    fails = fails + assert_eq("to_int_passthru", to_int(5), 5)
    fails = fails + assert_eq("to_int_bad_is_nil", to_int("abc"), nil)
    fails
}

fun test_to_float() {
    fails = 0
    fails = fails + assert_eq("to_float_str", to_float("3.14"), 3.14)
    fails = fails + assert_eq("to_float_int", to_float(5), 5.0)
    fails = fails + assert_eq("to_float_bad_is_nil", to_float("nope"), nil)
    fails
}

fun test_uuid() {
    fails = 0
    fails = fails + assert_eq("uuid_len", string_length(uuid()), 36)
    fails = fails + assert_eq("uuid_unique", uuid() != uuid(), true)
    fails
}

fun test_now_iso() {
    # ISO-8601 UTC "YYYY-MM-DDThh:mm:ssZ" is exactly 20 chars and ends in Z.
    s = now_iso()
    fails = assert_eq("now_iso_len", string_length(s), 20)
    fails = fails + assert_eq("now_iso_ends_z", string_ends_with(s, "Z"), true)
    fails
}

fun main() {
    fails = 0
    fails = fails + test_to_int()
    fails = fails + test_to_float()
    fails = fails + test_uuid()
    fails = fails + test_now_iso()
    if (fails == 0) { print("OK conversions 12/12") ; sys_exit(0) }
    else { print("FAIL conversions " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
