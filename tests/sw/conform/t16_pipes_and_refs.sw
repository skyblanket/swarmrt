module Conform_pipes_and_refs

# Conformance: every pipe target and every function-by-name value must mean
# the same thing on both paths. Before: the interpreter returned the piped
# value untouched for a bare target (`|> print`, `|> Std.sum`, `|> f`),
# the compiler did the same for `|> fun(x) {...}`, the interpreter returned
# nil for a module function passed as a value, and `Std.sum` without parens
# parsed as a map lookup on an undefined variable.

import Std

fun double(x) { x * 2 }

fun main() {
    [3, 1, 2] |> length |> to_string |> print
    "hi" |> string_upper |> print
    7 |> fun(x) { x * 3 } |> print
    sq = fun(x) { x * x }
    print(5 |> sq)
    print(map(double, [1, 2, 3]))
    print([1, 2, 3, 4] |> filter(fun(x) { x % 2 == 0 }) |> Std.sum)
    total = Std.sum
    print(total([10, 20]))
    summer = fun(xs) { xs |> Std.sum }
    print(summer([1, 1, 1]))
}
