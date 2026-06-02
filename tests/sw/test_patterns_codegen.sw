module Test_patterns_codegen

# Regression tests for map-pattern and cons-tail codegen parity with the
# interpreter (pattern_match in src/swarmrt_lang.c).
#
# Two bugs these guard against:
#   1. emit_pattern_cond/bind had NO N_MAP case, so `%{name: n}` matched
#      EVERY value and `n` was left unbound — both %{name:"alice"} and
#      %{other:1} wrongly printed "name=n".
#   2. emit_pattern_cond's N_LIST_CONS case checked only count>=1 + the
#      HEAD, never the tail pattern, so `[h | []]` matched any non-empty
#      list and `[h | [a,b]]` ignored the tail shape.
#
# KNOWN LIMITATION (intentionally NOT tested as a pass): a multi-element
# cons like `[a, b | rest]` is a clean parse error ("expected ']', got
# '|'"). That is out of scope / lean — do not implement it.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# ---- map patterns ----

fun named(m) {
    case m {
        %{name: n} -> f"name={n}"
        _          -> "no-match"
    }
}

# present key binds the VALUE (not the binder name)
fun test_map_present_binds_value() {
    assert_eq("map_present_binds_value", named(%{name: "alice"}), "name=alice")
}

# a key that is NOT present must fall through to _ (the original bug
# matched every map and printed "name=n")
fun test_map_absent_falls_through() {
    assert_eq("map_absent_falls_through", named(%{other: 1}), "no-match")
}

# a non-map value must not match a map pattern
fun test_map_pattern_rejects_nonmap() {
    assert_eq("map_rejects_nonmap", named(42), "no-match")
}

# multi-key map pattern: ALL keys must be present
fun pair(m) {
    case m {
        %{a: x, b: y} -> f"{x}+{y}"
        _             -> "no"
    }
}
fun test_map_multikey_all_present() {
    assert_eq("map_multikey_all_present", pair(%{a: 2, b: 3}), "2+3")
}
fun test_map_multikey_one_missing() {
    assert_eq("map_multikey_one_missing", pair(%{a: 2}), "no")
}

# nested map pattern binds the INNER value
fun nested(m) {
    case m {
        %{user: %{id: i}} -> f"id={i}"
        _                 -> "no-user"
    }
}
fun test_nested_map_binds_inner() {
    assert_eq("nested_map_binds_inner", nested(%{user: %{id: 42}}), "id=42")
}
fun test_nested_map_inner_key_missing() {
    assert_eq("nested_map_inner_missing", nested(%{user: %{name: "x"}}), "no-user")
}
fun test_nested_map_outer_key_missing() {
    assert_eq("nested_map_outer_missing", nested(%{other: 1}), "no-user")
}

# JSON idiom: atom pattern key matches a string-keyed map (atom<->string
# equivalence in sw_val_map_get — the flagship LLM-response use case).
fun test_map_atom_pattern_string_key() {
    m = map_put(%{}, "name", "bob")   # string key
    assert_eq("map_atom_pat_string_key", named(m), "name=bob")
}

# ---- cons-tail patterns ----

# [h | t] binds head + tail
fun headtail(l) {
    case l {
        [h | t] -> f"h={h} t={to_string(t)}"
        _       -> "empty"
    }
}
fun test_cons_binds_head_and_tail() {
    assert_eq("cons_binds_head_tail", headtail([1, 2, 3]), "h=1 t=[2, 3]")
}
fun test_cons_empty_falls_through() {
    assert_eq("cons_empty_falls_through", headtail([]), "empty")
}

# [h | []] matches ONLY a singleton list (the soundness bug: it used to
# match any non-empty list)
fun only_single(l) {
    case l {
        [h | []] -> f"single:{h}"
        _        -> "not-single"
    }
}
fun test_cons_singleton_matches() {
    assert_eq("cons_singleton_matches", only_single([7]), "single:7")
}
fun test_cons_singleton_rejects_longer() {
    assert_eq("cons_singleton_rejects_longer", only_single([1, 2, 3]), "not-single")
}

# [h | [a, b]] respects the tail SHAPE (must be a 2-element tail)
fun head_then_pair(l) {
    case l {
        [h | [a, b]] -> f"h={h} a={a} b={b}"
        _            -> "no"
    }
}
fun test_cons_tail_shape_matches() {
    assert_eq("cons_tail_shape_matches", head_then_pair([9, 8, 7]), "h=9 a=8 b=7")
}
fun test_cons_tail_shape_rejects() {
    # 4 elements -> head + 3-element tail, does NOT match a 2-element tail
    assert_eq("cons_tail_shape_rejects", head_then_pair([9, 8, 7, 6]), "no")
}

fun main() {
    fails = 0
    fails = fails + test_map_present_binds_value()
    fails = fails + test_map_absent_falls_through()
    fails = fails + test_map_pattern_rejects_nonmap()
    fails = fails + test_map_multikey_all_present()
    fails = fails + test_map_multikey_one_missing()
    fails = fails + test_nested_map_binds_inner()
    fails = fails + test_nested_map_inner_key_missing()
    fails = fails + test_nested_map_outer_key_missing()
    fails = fails + test_map_atom_pattern_string_key()
    fails = fails + test_cons_binds_head_and_tail()
    fails = fails + test_cons_empty_falls_through()
    fails = fails + test_cons_singleton_matches()
    fails = fails + test_cons_singleton_rejects_longer()
    fails = fails + test_cons_tail_shape_matches()
    fails = fails + test_cons_tail_shape_rejects()
    if (fails == 0) { print("OK patterns_codegen 15/15") ; sys_exit(0) }
    else { print("FAIL patterns_codegen " ++ to_string(fails)) ; sys_exit(1) }
}
