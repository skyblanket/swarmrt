module Main

import Std

fun main() {
    result = [1,2,3,4,5,6,7,8,9,10]
        |> filter(fun(x) { x % 2 == 0 })
        |> map(fun(x) { x * x })
        |> Std.sum()
    print(result)
}
