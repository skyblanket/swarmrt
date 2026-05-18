module Test_errors

# Error-handling and loud-fail behaviour. Tests that:
#   - expect() lets a value through when non-nil
#   - try/catch catches error() raised in the body
#   - try/catch returns the body's value when no error fires
#
# We can't test panic() / hd([]) / divzero here because they exit(1)
# unconditionally and the test driver expects exit(0). Those are
# exercised by the dedicated probes in /tmp during development.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun test_expect_pass_through() {
    user = %{name: "alice"}
    name = expect(map_get(user, 'name'), "no name")
    assert_eq("expect_pass_through", name, "alice")
}

fun test_try_catch_caught() {
    r = try {
        error("boom")
        "never"
    } catch e {
        f"caught: {e}"
    }
    assert_eq("try_caught", r, "caught: boom")
}

fun test_try_catch_uncaught_returns_body() {
    r = try { 42 } catch e { "n/a" }
    assert_eq("try_no_error", r, 42)
}

fun test_two_trys_share_err_name() {
    # Regression: the scope-shadowing fix from 2026-05-15 lets us reuse
    # `e` as the catch variable in sibling try/catch blocks.
    r1 = try { error("a") } catch e { f"got: {e}" }
    r2 = try { error("b") } catch e { f"got: {e}" }
    fails = assert_eq("two_trys_a", r1, "got: a")
    fails + assert_eq("two_trys_b", r2, "got: b")
}

fun main() {
    fails = 0
    fails = fails + test_expect_pass_through()
    fails = fails + test_try_catch_caught()
    fails = fails + test_try_catch_uncaught_returns_body()
    fails = fails + test_two_trys_share_err_name()
    if (fails == 0) { print("OK errors 5/5") ; sys_exit(0) }
    else { print("FAIL errors " ++ to_string(fails)) ; sys_exit(1) }
}
