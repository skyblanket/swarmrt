module MapReduce

fun main() {
  nums = [1, 2, 3, 4, 5]

  doubled = map(fun(x) { x * 2 }, nums)
  print("map x*2:", doubled)

  total = reduce(fun(acc, x) { acc + x }, nums, 0)
  print("reduce sum:", total)

  evens = filter(fun(x) { x % 2 == 0 }, nums)
  print("filter (evens only):", evens)

  pdoubled = pmap(fun(x) { x * 2 }, nums)
  print("pmap x*2:", pdoubled)

  product = reduce(fun(acc, x) { acc * x }, nums, 1)
  print("reduce product:", product)
}
