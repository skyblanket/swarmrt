module Main

import Std

fun main() {
    for i in Std.range(1, 16) {
        case i {
            n when n % 15 == 0 -> print("FizzBuzz")
            n when n % 3 == 0 -> print("Fizz")
            n when n % 5 == 0 -> print("Buzz")
            n -> print(to_string(n))
        }
    }
}
