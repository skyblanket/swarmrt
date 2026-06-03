module Main

import Std

fun main() {
    for i in Std.range(1, 16) {
        case i {
            x when x % 15 == 0 -> print("FizzBuzz")
            x when x % 3 == 0  -> print("Fizz")
            x when x % 5 == 0  -> print("Buzz")
            _                  -> print(i)
        }
    }
}
