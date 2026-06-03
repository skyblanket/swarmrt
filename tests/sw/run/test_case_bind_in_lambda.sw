# test_case_bind_in_lambda.sw (interp twin) — case-arm var binding inside a
# lambda body must behave identically in the tree-walking interpreter and the
# compiled path. The compiled path was the one that broke (phantom lambda
# capture → "use of undeclared identifier"); the interpreter already handled
# it, so this guards against future drift in EITHER direction.
#
# Interp half of tests/sw/test_case_bind_in_lambda.sw.
# Self-checking: prints "CASE_BIND_LAMBDA_INTERP_OK" + sys_exit(0).

module Test_case_bind_in_lambda_interp

fun chk(name, actual, expected) {
    if (actual == expected) { 0 }
    else {
        print("CASE_BIND_LAMBDA_INTERP_FAIL " ++ name ++ ": expected " ++
              to_string(expected) ++ ", got " ++ to_string(actual))
        1
    }
}

fun apply1(f, x) { f(x) }

fun main() {
    fails = 0

    # bare var-bind arm inside a lambda
    f1 = fn(x) { case x { 0 -> "zero" ; m -> "got " ++ to_string(m) } }
    fails = fails + chk("var_bind", apply1(f1, 7), "got 7")

    # tuple-pattern binders inside a lambda case arm
    f2 = fn(x) { case x { {'pair', a, b} -> a + b ; _ -> 0 } }
    fails = fails + chk("tuple_bind", apply1(f2, {'pair', 20, 22}), 42)

    # genuine capture survives alongside binders
    base = 100
    f3 = fn(x) { case x { {'add', n} -> base + n ; n -> base + n } }
    fails = fails + chk("capture_plus_bind",
                        apply1(f3, {'add', 5}) + apply1(f3, 8), 213)

    # nested case inside a lambda
    g = fn(y) {
        case y {
            0 -> "z"
            m -> case m { 1 -> "one" ; k -> "k" ++ to_string(k) }
        }
    }
    fails = fails + chk("nested_case_one", apply1(g, 1), "one")
    fails = fails + chk("nested_case_other", apply1(g, 9), "k9")

    if (fails == 0) { print("CASE_BIND_LAMBDA_INTERP_OK") ; sys_exit(0) }
    else { sys_exit(1) }
}
