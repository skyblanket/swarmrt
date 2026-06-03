module Main

import Std

fun main() {
    l = [3, 1, 4, 1, 5, 9, 2, 6]
    print(f"sum={Std.sum(l)}")
    print(f"length={length(l)}")
    print(f"max={Std.max_by(l, fun(x) { x })}")
}
