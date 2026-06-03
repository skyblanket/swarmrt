module Main

import Std

fun main() {
    for i in Std.range(1, 16) {
        case {i % 3, i % 5} {
            {0, 0} -> print("FizzBuzz")
            {0, _} -> print("Fizz")
            {_, 0} -> print("Buzz")
            _      -> print(i)
        }
    }
}
