# test_lambda_args.sw — `fn(params){ body }` is a first-class lambda literal
# usable in ANY expression position, not just as a `fun` assignment RHS / map
# value. This closes the first-try-writeability gap where an inline lambda was
# not a valid CALL ARGUMENT: `Cron.every(30000, fn(){ check_inbox() })` used to
# be a parse error ("expected ')', got '{'"). `fn` is a synonym for the `fun`
# keyword ONLY in lambda position; as a bare identifier / call it stays an
# ordinary name (the stdlib uses `fn` as a callback param all over).
#
# Compiled twin of tests/sw/run/test_lambda_args.sw — the two MUST agree.
# Self-checking: prints "OK lambda_args N/N" + sys_exit(0) on full pass.

module Test_lambda_args
import Std

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# A function that takes a lambda as its first argument and applies it.
fun apply1(f, x) { f(x) }

# `fn` as an ORDINARY parameter name, called as `fn(x)`. This MUST stay a plain
# call — proving the lambda lookahead is structural (`fn ( ... ) {`), not a
# blanket keyword grab that would break every stdlib callback.
fun call_fn_param(fn, x) { fn(x) }

# A lambda RETURNED from a function (lambda in return/tail position).
fun adder(n) { fn(x){ x + n } }

# 1. lambda as a direct call argument
fun test_arg() { assert_eq("arg", apply1(fn(x){ x * 2 }, 21), 42) }

# 2. lambda passed to a stdlib higher-order builtin (map)
fun test_map() { assert_eq("map", map(fn(x){ x + 1 }, [1, 2, 3]), [2, 3, 4]) }

# 3. lambda in a list literal
fun test_list() {
    fns = [fn(){ 1 }, fn(){ 2 }]
    a = apply1(hd(fns), 0)        # ignores arg, returns 1
    b = apply1(hd(tl(fns)), 0)    # returns 2
    assert_eq("list", a + b, 3)
}

# 4. lambda as a map value
fun test_map_value() {
    m = %{ double: fn(x){ x * 2 } }
    f = map_get(m, 'double')
    assert_eq("map_value", f(20), 40)
}

# 5. lambda returned from a function then called
fun test_returned() {
    add10 = adder(10)
    assert_eq("returned", add10(5), 15)
}

# 6. `fn` as a plain parameter + `fn(x)` call must NOT be parsed as a lambda
fun test_fn_as_param() {
    assert_eq("fn_as_param", call_fn_param(fn(y){ y + 100 }, 5), 105)
}

# 7. fn and fun produce identical behaviour (interchangeable introducers)
fun test_fn_eq_fun() {
    using_fn  = apply1(fn(x){ x - 1 },  10)
    using_fun = apply1(fun(x){ x - 1 }, 10)
    assert_eq("fn_eq_fun", using_fn, using_fun)
}

# 8. nested lambda inside a lambda body (lambda in expression position, deep)
fun test_nested() {
    make = fn(n){ fn(x){ x + n } }
    inc = apply1(make, 7)
    assert_eq("nested", inc(35), 42)
}

# 9. lambda passed to Std.task_stream (the documented concurrent-map shape).
#    Compiled-only assertion: the worker loop recurses, which the tree-walking
#    interpreter caps (no TCO) — the run/ twin omits this one. Compiled is the
#    supported path for this primitive.
fun test_task_stream() {
    r = Std.task_stream([1, 2, 3], fn(x){ x * 2 }, %{max_concurrency: 2})
    # r is [{'ok',2},{'ok',4},{'ok',6}]; sum the values out.
    vals = map(fn(t){ elem(t, 1) }, r)
    assert_eq("task_stream", Std.sum(vals), 12)
}

fun main() {
    fails = 0
    fails = fails + test_arg()
    fails = fails + test_map()
    fails = fails + test_list()
    fails = fails + test_map_value()
    fails = fails + test_returned()
    fails = fails + test_fn_as_param()
    fails = fails + test_fn_eq_fun()
    fails = fails + test_nested()
    fails = fails + test_task_stream()
    if (fails == 0) { print("OK lambda_args 9/9") ; sys_exit(0) }
    else { print("FAIL lambda_args " ++ to_string(fails)) ; sys_exit(1) }
}
