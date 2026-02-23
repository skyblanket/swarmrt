module MultiMain

fun main() {
  print("=== Multi-module test ===")

  a = MathLib.square(7)
  print("MathLib.square(7) =", a)

  b = MathLib.double(21)
  print("MathLib.double(21) =", b)

  c = MathLib.factorial(5)
  print("MathLib.factorial(5) =", c)

  nums = [1, 2, 3, 4, 5]
  d = MathLib.sum_list(nums)
  print("MathLib.sum_list([1..5]) =", d)

  print("=== Done ===")
}
