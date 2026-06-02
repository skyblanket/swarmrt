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
#   3. Multi-element cons `[a, b | rest]` (parsed as a right-nested cons
#      chain cons(a, cons(b, rest))) must match a list of length >= 2,
#      binding a=elem0, b=elem1, rest=drop(2) — identically in the
#      interpreter and the compiler.

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

# ---- multi-element cons [a, b | rest] ----

# [a, b | rest] binds first two + rest, requires length >= 2
fun two_plus_rest(l) {
    case l {
        [a, b | rest] -> f"a={a} b={b} rest={to_string(rest)}"
        _             -> "lt2"
    }
}
fun test_abrest_binds() {
    assert_eq("abrest_binds", two_plus_rest([1, 2, 3, 4]), "a=1 b=2 rest=[3, 4]")
}
fun test_abrest_exactly_two() {
    # exactly 2 elements: matches, rest is the empty list
    assert_eq("abrest_exactly_two", two_plus_rest([5, 6]), "a=5 b=6 rest=[]")
}
fun test_abrest_len1_falls_through() {
    assert_eq("abrest_len1_falls_through", two_plus_rest([9]), "lt2")
}
fun test_abrest_empty_falls_through() {
    assert_eq("abrest_empty_falls_through", two_plus_rest([]), "lt2")
}

# nested heads: [{k, v}, second | rest]
fun pairhead(l) {
    case l {
        [{k, v}, second | rest] -> f"k={k} v={v} s={second} r={to_string(rest)}"
        _                       -> "no"
    }
}
fun test_abrest_nested_head_matches() {
    assert_eq("abrest_nested_head_matches",
              pairhead([{"a", 1}, 99, 100, 101]), "k=a v=1 s=99 r=[100, 101]")
}
fun test_abrest_nested_head_rejects() {
    # head 0 is not a 2-tuple -> no match
    assert_eq("abrest_nested_head_rejects", pairhead([7, 99, 100]), "no")
}

# three-element cons [a, b, c | rest] requires length >= 3
fun three_plus_rest(l) {
    case l {
        [a, b, c | rest] -> f"{a}{b}{c}|{to_string(rest)}"
        _                -> "lt3"
    }
}
fun test_abcrest_binds() {
    assert_eq("abcrest_binds", three_plus_rest([1, 2, 3, 4, 5]), "123|[4, 5]")
}
fun test_abcrest_len2_falls_through() {
    assert_eq("abcrest_len2_falls_through", three_plus_rest([1, 2]), "lt3")
}

# construction (expression) position: [a, b | rest] builds the joined list
fun test_abrest_construction() {
    rest = [3, 4]
    assert_eq("abrest_construction", to_string([1, 2 | rest]), "[1, 2, 3, 4]")
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
    fails = fails + test_abrest_binds()
    fails = fails + test_abrest_exactly_two()
    fails = fails + test_abrest_len1_falls_through()
    fails = fails + test_abrest_empty_falls_through()
    fails = fails + test_abrest_nested_head_matches()
    fails = fails + test_abrest_nested_head_rejects()
    fails = fails + test_abcrest_binds()
    fails = fails + test_abcrest_len2_falls_through()
    fails = fails + test_abrest_construction()
    if (fails == 0) { print("OK patterns_codegen 24/24") ; sys_exit(0) }
    else { print("FAIL patterns_codegen " ++ to_string(fails)) ; sys_exit(1) }
}
