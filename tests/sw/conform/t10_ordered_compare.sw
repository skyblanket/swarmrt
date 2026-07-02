module Conform_compare

# Conformance: ordered comparison (< > <= >=) on strings AND ints must
# agree interpreter-vs-compiled. Strings previously returned nil in the
# interpreter; fixed by porting compiled _op_cmp's string-lexicographic
# branch. (Full BEAM total-order across mixed/atom/list types is still
# deferred — this gate only pins the string<string and int cases.)

fun main() {
    a = "apple"
    b = "banana"
    print(f"str_lt={a < b} gt={b > a} le={a <= a} ge={a >= b}")
    print(f"str_eqcmp={a < a} {a <= b} {b >= a}")
    print(f"int_cmp={2 < 10} {10 <= 10} {5 > 3} {3 >= 9}")
    print(f"eq={a == a} {a == b} {1 == 1} {1 != 2}")
}
