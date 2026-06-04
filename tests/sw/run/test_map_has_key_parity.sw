module Test_map_has_key_parity

# Regression: map_has_key must agree with map_get, and must behave IDENTICALLY
# in the interpreter (swc run) and the compiled backend. The interpreter's
# map_has_key used a bare sw_val_equal loop with no atom-vs-string fallback,
# so on a json_decode'd map a string-keyed query returned 'false' even though
# map_get found the value (atom key) — interp said false, compiled said true.
# Runs in BOTH backends (compiled here + tests/sw/run/ copy) so the parity is
# locked from both sides.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# An atom-keyed map literal: map_has_key must be atom-vs-string tolerant
# (mirroring map_get), so BOTH the atom and the string query find the key.
fun test_atom_literal_map() {
    m = %{name: "Sam", age: 30}
    fails = 0
    fails = fails + assert_eq("atom_key_atom_query", map_has_key(m, 'name'), 'true')
    fails = fails + assert_eq("atom_key_string_query", map_has_key(m, "name"), 'true')
    fails = fails + assert_eq("absent_key", map_has_key(m, 'missing'), 'false')
    fails
}

# The exact bug: a json_decode'd map. Query with both a string key and an atom
# key — both must report present, and must agree with map_get finding a value.
fun test_json_decoded_map() {
    back = json_decode("{\"id\":42,\"name\":\"deploy\"}")
    fails = 0
    fails = fails + assert_eq("json_string_query", map_has_key(back, "id"), 'true')
    fails = fails + assert_eq("json_atom_query", map_has_key(back, 'id'), 'true')
    fails = fails + assert_eq("json_absent", map_has_key(back, "nope"), 'false')
    # The invariant: has_key 'true' <=> map_get returns a non-nil value.
    got = map_get(back, "name")
    fails = fails + assert_eq("get_finds_it", got == nil, false)
    fails = fails + assert_eq("haskey_agrees_with_get", map_has_key(back, "name"), 'true')
    fails
}

fun main() {
    fails = 0
    fails = fails + test_atom_literal_map()
    fails = fails + test_json_decoded_map()
    if (fails == 0) { print("OK map_has_key_parity 8/8") ; sys_exit(0) }
    else { print("FAIL map_has_key_parity " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
