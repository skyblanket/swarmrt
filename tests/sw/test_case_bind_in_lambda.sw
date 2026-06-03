# test_case_bind_in_lambda.sw — a `case` arm that BINDS a variable (`m -> m`)
# inside a LAMBDA body must compile and run. Regression for the lambda-capture
# bug where a case-arm / receive-clause / catch pattern binding was mistaken
# for a free variable to capture from the enclosing scope: the lambda gained a
# phantom capture param and the call site read a nonexistent outer name —
#   src/X.sw:N: use of undeclared identifier 'm'
# (works fine in a top-level fun; only broke in lambda scope). The fix adds
# pattern-bound names to the lambda's local-name set alongside assignment
# targets, so they're never captured.
#
# Compiled twin of tests/sw/run/test_case_bind_in_lambda.sw — the two MUST
# agree. Self-checking: prints "OK case_bind_in_lambda N/N" + sys_exit(0).

module Test_case_bind_in_lambda

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun apply1(f, x) { f(x) }

# 1. bare var-bind arm inside a lambda (the headline gap).
fun test_var_bind() {
    f = fn(x) { case x { 0 -> "zero" ; m -> "got " ++ to_string(m) } }
    assert_eq("var_bind", apply1(f, 7), "got 7")
}

# 2. tuple-pattern binders inside a lambda case arm.
fun test_tuple_bind() {
    f = fn(x) { case x { {'pair', a, b} -> a + b ; _ -> 0 } }
    assert_eq("tuple_bind", apply1(f, {'pair', 20, 22}), 42)
}

# 3. genuine capture (`base`) MUST survive alongside case-arm binders.
fun test_capture_plus_bind() {
    base = 100
    f = fn(x) { case x { {'add', n} -> base + n ; n -> base + n } }
    r1 = apply1(f, {'add', 5})   # 105
    r2 = apply1(f, 8)            # 108
    assert_eq("capture_plus_bind", r1 + r2, 213)
}

# 4. nested case (binder inside a binder's arm) inside a lambda.
fun test_nested_case() {
    g = fn(y) {
        case y {
            0 -> "z"
            m -> case m { 1 -> "one" ; k -> "k" ++ to_string(k) }
        }
    }
    assert_eq("nested_case_one", apply1(g, 1), "one")
}
fun test_nested_case2() {
    g = fn(y) {
        case y {
            0 -> "z"
            m -> case m { 1 -> "one" ; k -> "k" ++ to_string(k) }
        }
    }
    assert_eq("nested_case_other", apply1(g, 9), "k9")
}

fun main() {
    fails = 0
    fails = fails + test_var_bind()
    fails = fails + test_tuple_bind()
    fails = fails + test_capture_plus_bind()
    fails = fails + test_nested_case()
    fails = fails + test_nested_case2()
    if (fails == 0) { print("OK case_bind_in_lambda 5/5") ; sys_exit(0) }
    else { print("FAIL case_bind_in_lambda " ++ to_string(fails)) ; sys_exit(1) }
}
