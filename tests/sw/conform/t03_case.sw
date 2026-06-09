module Conform_case

# Conformance: case dispatch — literals, guards (including the
# shadowed-scrutinee name), tuple/map/cons patterns, fallthrough on
# guard rejection, `_` default, case-as-RHS.

fun classify(n) {
    case n {
        0 -> 'zero'
        n when n > 100 -> 'big'
        n when n > 0   -> 'small'
        _ -> 'negative'
    }
}

fun shape(v) {
    case v {
        {'point', x, y}    -> f"point({x},{y})"
        %{kind: k}         -> f"map-kind={k}"
        [h | _]            -> f"list-head={h}"
        "lit"              -> "literal-string"
        _                  -> "other"
    }
}

fun main() {
    print(f"{classify(0)} {classify(5)} {classify(150)} {classify(0 - 4)}")
    print(shape({'point', 3, 4}))
    print(shape(%{kind: 'demo'}))
    print(shape([7, 8]))
    print(shape("lit"))
    print(shape(99))
    r = case 7 { n when n > 5 -> "gt5" ; _ -> "le5" }
    print(r)
}
