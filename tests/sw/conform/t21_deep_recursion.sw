module Conform_deep_recursion

# Conformance: a NON-tail recursion 10,000 frames deep must work and print
# the same result in the interpreter (`swc run`) and a compiled binary
# (`swc build`). Each call keeps its frame alive until the way back up, so
# this exercises the C stack on both paths (no TCO applies here).

fun sum_to(n) {
    if (n == 0) { 0 } else { n + sum_to(n - 1) }
}

fun count_down(n) {
    if (n == 0) { [] } else { [n] ++ count_down(n - 1) }
}

fun main() {
    print(sum_to(10000))
    print(length(count_down(10000)))
    print(hd(count_down(10000)))
}
