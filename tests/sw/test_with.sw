# test_with.sw — the `with` error-chain construct.
#
# `with p1 <- e1, p2 <- e2, ... { B } else { o -> E }` desugars at PARSE
# TIME into nested `case`, so the interpreter (`swc test` / `swc run`) and
# the compiled binary agree by construction. run_tests.sh compiles+runs
# this file; tests/sw/run/test_with_run.sw runs the same logic through the
# interpreter, proving interp==compiled.
#
# Covers:
#   1. happy path — every bind matches, body runs and threads values through
#   2. mid-chain mismatch — a later bind fails, else fires with the value
#   3. first-bind mismatch — the very first bind fails
#   4. single-bind degenerate case + `_` discard pattern in the else arm

module Test_with
export [main]

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# A 2-step pipeline that returns the summed doubled value on success, or a
# tagged error reason on any failure. Mirrors the tool/LLM ladder shape.
fun pipeline(a, b) {
    with {'ok', x} <- step(a),
         {'ok', y} <- step(b) {
        {'ok', x + y}
    } else {
        other -> {'failed', other}
    }
}

fun step(v) {
    if (v < 0) { {'err', v} }
    else { {'ok', v * 2} }
}

fun fetch(tag) {
    case tag {
        'ok' -> %{status: 200}
        _    -> %{status: 500}
    }
}

fun main() {
    fails = 0

    # 1. happy path: both steps ok → body binds x=4, y=10 → {'ok', 14}
    fails = fails + assert_eq("happy_threads_both_binds", pipeline(2, 5), {'ok', 14})

    # 2. mid-chain mismatch: second step fails → else fires with {'err', -3}
    fails = fails + assert_eq("midchain_else_gets_value", pipeline(2, 0 - 3), {'failed', {'err', 0 - 3}})

    # 3. first-bind mismatch: first step fails → else fires immediately
    fails = fails + assert_eq("first_bind_else_gets_value", pipeline(0 - 1, 5), {'failed', {'err', 0 - 1}})

    # 4. single-bind with: degenerates to one case, still binds + falls back
    r1 = with {'ok', n} <- step(7) { n } else { _ -> 'nope' }
    fails = fails + assert_eq("single_bind_happy", r1, 14)
    r2 = with {'ok', n} <- step(0 - 9) { n } else { e -> e }
    fails = fails + assert_eq("single_bind_else_binds_value", r2, {'err', 0 - 9})

    # 5. else arm with `_` (discard the non-matching value); map patterns
    r3 = with %{status: 200} <- fetch('ok'),
              %{status: 200} <- fetch('bad') {
        "all 200"
    } else {
        _ -> "not ok"
    }
    fails = fails + assert_eq("discard_pattern_else", r3, "not ok")

    if (fails == 0) {
        print("OK test_with 6/6")
        sys_exit(0)
    } else {
        print("FAIL test_with " ++ to_string(fails) ++ " failed")
        sys_exit(1)
    }
}
