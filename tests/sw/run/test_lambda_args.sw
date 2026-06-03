# test_lambda_args.sw (interp twin) — `fn(params){ body }` lambda literals must
# behave identically in the tree-walking interpreter (`swc run`) and the
# compiled path. This is the interp half of tests/sw/test_lambda_args.sw; it
# omits only the Std.task_stream case (its worker loop recurses and the
# interpreter has no TCO — that case is asserted compiled-side). Guards against
# parser/interp drift on lambdas in call-argument / list / map-value / return
# position. Self-checking: prints "LAMBDA_ARGS_INTERP_OK" + sys_exit(0).

module Test_lambda_args_interp
import Std

fun chk(name, actual, expected) {
    if (actual == expected) { 0 }
    else {
        print("LAMBDA_ARGS_INTERP_FAIL " ++ name ++ ": expected " ++
              to_string(expected) ++ ", got " ++ to_string(actual))
        1
    }
}

fun apply1(f, x) { f(x) }
fun call_fn_param(fn, x) { fn(x) }   # `fn` as a plain param name + call
fun adder(n) { fn(x){ x + n } }      # lambda in return position

fun main() {
    fails = 0

    # lambda as a call argument
    fails = fails + chk("arg", apply1(fn(x){ x * 2 }, 21), 42)

    # lambda to a higher-order builtin
    fails = fails + chk("map", map(fn(x){ x + 1 }, [1, 2, 3]), [2, 3, 4])

    # lambda in a list literal
    fns = [fn(){ 1 }, fn(){ 2 }]
    fails = fails + chk("list", apply1(hd(fns), 0) + apply1(hd(tl(fns)), 0), 3)

    # lambda as a map value
    m = %{ double: fn(x){ x * 2 } }
    dbl = map_get(m, 'double')
    fails = fails + chk("map_value", dbl(20), 40)

    # lambda returned from a function then called
    add10 = adder(10)
    fails = fails + chk("returned", add10(5), 15)

    # `fn` as a plain parameter + `fn(x)` call must stay a call, not a lambda
    fails = fails + chk("fn_as_param", call_fn_param(fn(y){ y + 100 }, 5), 105)

    # fn and fun are interchangeable lambda introducers
    fails = fails + chk("fn_eq_fun",
                        apply1(fn(x){ x - 1 }, 10),
                        apply1(fun(x){ x - 1 }, 10))

    # nested lambda inside a lambda body
    make = fn(n){ fn(x){ x + n } }
    inc = apply1(make, 7)
    fails = fails + chk("nested", inc(35), 42)

    if (fails == 0) { print("LAMBDA_ARGS_INTERP_OK") ; sys_exit(0) }
    else { sys_exit(1) }
}
