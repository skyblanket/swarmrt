module Conform_builtin_values

import Std

# Conformance: a builtin named as a value (not called) is wrapped in a
# closure of its arity, so `map(to_string, xs)` works and behaves the same
# on both paths. A local binding of the same name still shadows the builtin.

fun shadowed() {
    length = 7
    length
}

fun main() {
    xs = [1, 2, 3]
    print(Std.map(xs, to_string))
    print(map(["ab", "c"], string_upper))
    print(Std.map(["x", "yz"], length))
    print(Std.map(["a", "bb", "ccc"], length) |> Std.sum)

    upper = string_upper
    print(upper("sw"))

    print(reduce(list_append, [[1], [2]], []))
    print(shadowed())
}
