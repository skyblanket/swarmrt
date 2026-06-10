module Conform_std

import Std

# Conformance: the Std auto-import surface (list helpers, the
# map/filter/join aliases, string helpers) behaves identically on both
# paths — including either argument order for the aliases.

fun main() {
    print(Std.map([1, 2, 3], fun(x) { x * 10 }))
    print(Std.map(fun(x) { x + 1 }, [5, 6]))
    print(Std.filter([1, 2, 3, 4, 5, 6], fun(x) { x % 2 == 0 }))
    print(Std.join(["a", "b", "c"], "-"))
    print(Std.string_join([1, 2, 3], "+"))

    print(f"range={Std.range(1, 5)} take={Std.take([1, 2, 3, 4], 2)} drop={Std.drop([1, 2, 3, 4], 2)}")
    print(f"sort={Std.sort([3, 1, 2])} rev={Std.reverse([1, 2, 3])} uniq={Std.unique([1, 1, 2, 2, 3])}")
    print(f"sum={Std.sum([1, 2, 3, 4])} product={Std.product([2, 3, 4])} last={Std.last([7, 8, 9])}")
    print(f"any={Std.any([1, 3], fun(x) { x > 2 })} all={Std.all([1, 3], fun(x) { x > 0 })}")
    print(f"zip={Std.zip([1, 2], ['a', 'b'])} chunk={Std.chunk_every([1, 2, 3, 4, 5], 2)}")
    print(f"pad={Std.string_pad_left("7", 3, "0")} rep={Std.string_repeat("ab", 3)}")
}
