module Main

import Std

fun main() {
    l = [3, 1, 4, 1, 5, 9, 2, 6]
    
    sum_val = Std.sum(l)
    print(format("sum={}", sum_val))
    
    len_val = length(l)
    print(format("length={}", len_val))
    
    max_val = if (len_val > 0) {
        Std.max_by(l, fun(x, y) -> x > y)
    } else {
        0
    }
    print(format("max={}", max_val))
}
