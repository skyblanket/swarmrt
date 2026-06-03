module Main

import Std

fun main() {
    xs = [3, 1, 4, 1, 5, 9, 2, 6]
    print(f"sum={Std.sum(xs)}")
    print(f"length={length(xs)}")
    print(f"max={Std.max_by(xs, fun(x) { x })}")
}
