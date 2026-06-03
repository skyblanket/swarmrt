# test_case_tuple_body.sw — a bare TUPLE LITERAL as a case-arm body
# (`_ -> {'error', why}`) must parse as a tuple result, not as a statement
# block. This is the form LLMs reach for constantly; previously the `{` after
# `->` was always read as a block-open and the tuple's comma was a hard parse
# error ("unexpected ','"), forcing the ugly `-> ({'error', why})` workaround.
#
# The disambiguation is structural and conservative (see
# par_arm_brace_is_tuple): a brace body is a tuple iff it carries a TOP-LEVEL
# comma (which a statement block can never have). So this test also pins the
# block cases that must KEEP parsing as blocks — multi-statement blocks, a
# block wrapping a call (paren-commas), a block wrapping a nested tuple
# (depth-2 comma), and `%{...}` map results — to guard against the new tuple
# path swallowing them.
#
# Compiled twin of tests/sw/run/test_case_tuple_body.sw — the two MUST agree.
# Self-checking: prints "OK case_tuple_body N/N" + sys_exit(0).

module Test_case_tuple_body

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun add3(a, b, c) { a + b + c }

# The arm shapes under test, all in one case.
fun classify(x) {
    case x {
        0 -> {'error', 'zero'}                 # bare 2-tuple result (THE gap)
        1 -> { y = 10 ; y + x }                # multi-stmt block stays a block
        2 -> { add3(x, x, x) }                 # block wrapping a call (paren commas)
        3 -> { {'nested', x} }                 # block wrapping a tuple (depth-2 comma)
        4 -> {'triple', x, x}                  # 3-tuple result
        5 -> {'wrapped', add3(1, 2, 3)}        # tuple whose element is a call
        6 -> {'with_list', [10, 20, 30]}       # tuple whose element is a list
        _ -> {'default', x}                    # bare tuple default arm
    }
}

fun main() {
    fails = 0
    fails = fails + assert_eq("bare_tuple",   classify(0), {'error', 'zero'})
    fails = fails + assert_eq("block_multi",  classify(1), 11)
    fails = fails + assert_eq("block_call",   classify(2), 6)
    fails = fails + assert_eq("block_tuple",  classify(3), {'nested', 3})
    fails = fails + assert_eq("tuple3",       classify(4), {'triple', 4, 4})
    fails = fails + assert_eq("tuple_call",   classify(5), {'wrapped', 6})
    fails = fails + assert_eq("tuple_list",   classify(6), {'with_list', [10, 20, 30]})
    fails = fails + assert_eq("default_tuple", classify(99), {'default', 99})
    if (fails == 0) { print("OK case_tuple_body 8/8") ; sys_exit(0) }
    else { print("FAIL case_tuple_body " ++ to_string(fails)) ; sys_exit(1) }
}
