module Main

import Std

fun fizzbuzz(n) {
    case {n % 3 == 0, n % 5 == 0} {
        {'true', 'true'}  -> "FizzBuzz"
        {'true', 'false'} -> "Fizz"
        {'false', 'true'} -> "Buzz"
        _                 -> to_string(n)
    }
}

fun main() {
    for i in Std.range(1, 16) {
        print(fizzbuzz(i))
    }
}
