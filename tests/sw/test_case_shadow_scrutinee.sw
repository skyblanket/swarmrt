module Test_case_shadow_scrutinee

# Regression test for the shadowed-scrutinee codegen bug (June 2026 Fable
# audit). When a case arm re-binds the SAME name as the subject variable —
# the natural Erlang idiom — emit_case bound the arm-local straight off
# the subject identifier, emitting C self-initialization:
#
#     sw_val_t *n = n;        // new n shadows at its own declarator
#
# so the arm-local read indeterminate memory: guards silently failed
# (everything fell through to _) on a calm stack, and SIGSEGV'd inside
# _op_cmp on a dirty one. The interpreter was always correct, so this was
# also a compiled/interpreter divergence. Tuple, cons, and map sub-binds
# (`n->v.tuple.items[0]`) had the same hazard.
#
# The fix snapshots the subject into a fresh compiler temp before any arm
# binds. `with` desugars to case, so it is covered by the same change.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# ---- ident pattern, same name as subject, with guards ----

fun classify(n) {
    case n {
        0 -> 'zero'
        n when n > 100 -> 'big'
        n when n > 0   -> 'small'
        _ -> 'negative'
    }
}

fun test_shadow_guard_positive() {
    assert_eq("shadow_guard_small", classify(5), 'small')
}

fun test_shadow_guard_big() {
    assert_eq("shadow_guard_big", classify(150), 'big')
}

fun test_shadow_guard_zero_and_negative() {
    a = assert_eq("shadow_guard_zero", classify(0), 'zero')
    b = assert_eq("shadow_guard_negative", classify(0 - 3), 'negative')
    a + b
}

# ---- tuple sub-bind that re-uses the subject name ----

fun first_of(pair) {
    case pair {
        {pair, _} -> pair
        _ -> 'not_a_pair'
    }
}

fun test_shadow_tuple_item() {
    assert_eq("shadow_tuple_item", first_of({42, 'x'}), 42)
}

# ---- cons head that re-uses the subject name ----

fun head_of(xs) {
    case xs {
        [xs | _] -> xs
        _ -> 'empty'
    }
}

fun test_shadow_cons_head() {
    a = assert_eq("shadow_cons_head", head_of([7, 8, 9]), 7)
    b = assert_eq("shadow_cons_empty", head_of([]), 'empty')
    a + b
}

fun main() {
    failures = 0
    failures = failures + test_shadow_guard_positive()
    failures = failures + test_shadow_guard_big()
    failures = failures + test_shadow_guard_zero_and_negative()
    failures = failures + test_shadow_tuple_item()
    failures = failures + test_shadow_cons_head()
    if (failures == 0) {
        print("OK test_case_shadow_scrutinee 7/7")
        sys_exit(0)
    } else {
        print(f"FAIL test_case_shadow_scrutinee: {failures} failed")
        sys_exit(1)
    }
}
