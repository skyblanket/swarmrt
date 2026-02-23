module Lambda

fun main() {
  dbl = fun(x) { x * 2 }
  add = fun(x, y) { x + y }

  result = dbl(5)
  print("dbl(5) =", result)

  sum = add(3, 7)
  print("add(3, 7) =", sum)

  multiplier = fun(factor) {
    fun(x) { x * factor }
  }
  triple = multiplier(3)
  print("triple(10) =", triple(10))
}
