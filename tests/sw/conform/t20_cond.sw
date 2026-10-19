module Conform_cond

# Conformance: cond multi-way branching (ordered clauses, `_` catch-all,
# nil when nothing matches, tuple/block bodies, `cond` as a plain name).

fun classify(n) {
    cond {
        n < 0 -> "negative"
        n == 0 -> "zero"
        n < 10 -> { x = n * 2 ; f"small={x}" }
        _ -> "big"
    }
}

fun no_default(n) {
    cond {
        n == 1 -> 'one'
        n == 2 -> {'two', n}
    }
}

fun main() {
    print(classify(0 - 5))
    print(classify(0))
    print(classify(4))
    print(classify(50))
    print(no_default(1))
    print(no_default(2))
    print(no_default(3))
    cond = 7
    print(cond + 1)
    print(cond { 'false' -> 1 ; 'true' -> 2 })
}
