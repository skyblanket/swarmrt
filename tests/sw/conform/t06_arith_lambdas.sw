module Conform_arith

# Conformance: arithmetic semantics (int vs float division, mod,
# mixed-type promotion, comparisons), closures (capture), higher-order
# functions, the pipe operator, bounded recursion.
#
# Depth is 2000 here, NOT unbounded: the interpreter is a C-stack
# tree-walker with a stack guard and no TCO (documented), while compiled
# code TCO-loops self-calls flat. Unbounded-depth flatness is gated on
# the compiled path in tests/sw/test_tco_depth.sw.

fun make_adder(k) { fun(x) { x + k } }

fun count(n, i) { if (i >= n) { i } else { count(n, i + 1) } }

fun main() {
    print(f"intdiv={7 / 2} floatdiv={7.0 / 2.0} mixed={7 / 2.0} mod={7 % 3} neg={0 - 7}")
    print(f"cmp={3 > 2} {2 >= 2} {1 != 2} {(1 < 2) == 'true'}")
    print(f"float={3.14159 * 2.0} small={1.0 / 3.0}")

    add5 = make_adder(5)
    add_neg = make_adder(0 - 3)
    print(f"closures={add5(10)} {add_neg(add5(0))}")

    out = [1, 2, 3, 4, 5, 6]
        |> filter(fun(x) { x % 2 == 0 })
        |> map(fun(x) { x * x })
        |> reduce(fun(a, x) { a + x }, 0)
    print(f"pipeline={out}")

    print(f"recursion={count(400, 0)}")
}
