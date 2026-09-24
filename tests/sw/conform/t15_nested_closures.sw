module Conform_nested_closures

# Conformance: a lambda nested inside another lambda may capture variables
# from ANY enclosing scope. The compiled path used to capture only the
# innermost lambda's direct free variables, so `fun() { fun() { x } }`
# emitted a read of an undeclared C variable and failed to build.

fun make_adder(n) { fun(x) { fun(y) { x + y + n } } }

fun compose(f, g) { fun(x) { f(g(x)) } }

fun main() {
    base = 100
    add = make_adder(base)
    add1 = add(1)
    print(add1(2))
    k = 7
    f = fun() { fun() { fun() { k * 6 } } }
    f2 = f()
    f3 = f2()
    print(f3())
    inc = fun(x) { x + 1 }
    dbl = fun(x) { x * 2 }
    both = compose(inc, dbl)
    print(both(5))
    tag = "item"
    labels = map(fun(i) { map(fun(j) { f"{tag}-{i}-{j}" }, [1, 2]) }, [1, 2])
    print(labels)
    offset = 10
    nested = reduce(fun(acc, xs) { acc + reduce(fun(a, x) { a + x + offset }, xs, 0) }, [[1, 2], [3]], 0)
    print(nested)
}
