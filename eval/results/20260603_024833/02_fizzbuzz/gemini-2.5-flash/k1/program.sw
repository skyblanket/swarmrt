module Main

import Std

fun main() {
    for n in Std.range(1, 16) {
        cond3 = n % 3 == 0
        cond5 = n % 5 == 0

        if (cond3 and cond5) {
            print("FizzBuzz")
        } else if (cond3) {
            print("Fizz")
        } else if (cond5) {
            print("Buzz")
        } else {
            print(n)
        }
    }
}
