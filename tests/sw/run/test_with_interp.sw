# test_with_interp.sw — the `with` error-chain must behave identically in the
# tree-walking interpreter (`swc run`) and the compiled path. Since `with`
# desugars to nested `case` at PARSE TIME (shared front end, before either
# backend), interp==compiled is by construction — this guards that the desugar
# survives the interpreter's case/pattern-match machinery unchanged.
#
# Mirror of the compiled tests/sw/test_with.sw assertions. Self-checking:
# prints WITH_INTERP_OK and exits 0 only on full pass.

module Test_with_interp

fun chk(name, actual, expected) {
    if (actual == expected) { 0 }
    else {
        print("WITH_INTERP_FAIL " ++ name ++ ": expected " ++
              to_string(expected) ++ ", got " ++ to_string(actual))
        1
    }
}

fun step(v) {
    if (v < 0) { {'err', v} }
    else { {'ok', v * 2} }
}

fun pipeline(a, b) {
    with {'ok', x} <- step(a),
         {'ok', y} <- step(b) {
        {'ok', x + y}
    } else {
        other -> {'failed', other}
    }
}

fun main() {
    fails = 0
    fails = fails + chk("happy", pipeline(2, 5), {'ok', 14})
    fails = fails + chk("midchain", pipeline(2, 0 - 3), {'failed', {'err', 0 - 3}})
    fails = fails + chk("first", pipeline(0 - 1, 5), {'failed', {'err', 0 - 1}})

    r = with {'ok', n} <- step(0 - 9) { n } else { e -> e }
    fails = fails + chk("single_else", r, {'err', 0 - 9})

    if (fails == 0) { print("WITH_INTERP_OK") ; sys_exit(0) }
    else { sys_exit(1) }
}
