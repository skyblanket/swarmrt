module Main

import Std

fun main() {
    lst = [3, 1, 4, 1, 5, 9, 2, 6]
    print(f"sum={Std.sum(lst)}")
    print(f"length={length(lst)}")
    print(f"max={Std.max_by(lst, fun(x) { x })}")
}
