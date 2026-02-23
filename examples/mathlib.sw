module MathLib

fun square(x) {
  x * x
}

fun double(x) {
  x * 2
}

fun factorial(n) {
  if (n <= 1) {
    1
  } else {
    n * factorial(n - 1)
  }
}

fun sum_list(lst) {
  reduce(fun(acc, x) { acc + x }, lst, 0)
}
