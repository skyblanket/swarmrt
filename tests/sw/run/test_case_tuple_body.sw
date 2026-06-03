# test_case_tuple_body.sw (interp twin) — a bare tuple-literal case-arm body
# (`_ -> {'error', why}`) and the block edge cases must parse and evaluate
# identically in the tree-walking interpreter and the compiled path. Both
# paths share the same parser (par_arm_brace_is_tuple), so this guards against
# a future parser change diverging the two, plus interp-side tuple eval.
#
# Interp half of tests/sw/test_case_tuple_body.sw.
# Self-checking: prints "CASE_TUPLE_BODY_INTERP_OK" + sys_exit(0).

module Test_case_tuple_body_interp

fun chk(name, actual, expected) {
    if (actual == expected) { 0 }
    else {
        print("CASE_TUPLE_BODY_INTERP_FAIL " ++ name ++ ": expected " ++
              to_string(expected) ++ ", got " ++ to_string(actual))
        1
    }
}

fun add3(a, b, c) { a + b + c }

fun classify(x) {
    case x {
        0 -> {'error', 'zero'}
        1 -> { y = 10 ; y + x }
        2 -> { add3(x, x, x) }
        3 -> { {'nested', x} }
        4 -> {'triple', x, x}
        5 -> {'wrapped', add3(1, 2, 3)}
        6 -> {'with_list', [10, 20, 30]}
        _ -> {'default', x}
    }
}

fun main() {
    fails = 0
    fails = fails + chk("bare_tuple",    classify(0), {'error', 'zero'})
    fails = fails + chk("block_multi",   classify(1), 11)
    fails = fails + chk("block_call",    classify(2), 6)
    fails = fails + chk("block_tuple",   classify(3), {'nested', 3})
    fails = fails + chk("tuple3",        classify(4), {'triple', 4, 4})
    fails = fails + chk("tuple_call",    classify(5), {'wrapped', 6})
    fails = fails + chk("tuple_list",    classify(6), {'with_list', [10, 20, 30]})
    fails = fails + chk("default_tuple", classify(99), {'default', 99})
    if (fails == 0) { print("CASE_TUPLE_BODY_INTERP_OK") ; sys_exit(0) }
    else { sys_exit(1) }
}
