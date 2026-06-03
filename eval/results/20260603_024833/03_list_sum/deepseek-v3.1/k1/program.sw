module Main
import Std

fun main() {
    nums = [3, 1, 4, 1, 5, 9, 2, 6]
    print(f"sum={Std.sum(nums)}")
    print(f"length={length(nums)}")
    print(f"max={Std.max_by(nums, fun(x) { x })}")
}
