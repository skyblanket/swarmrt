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

  # `fn(...) { ... }` is the shorter, interchangeable lambda form. A lambda is
  # a first-class value usable in any expression position — including directly
  # as a call argument:
  doubled = map(fn(x) { x * 2 }, [1, 2, 3])
  print("map fn(x){x*2} =", to_string(doubled))
}
