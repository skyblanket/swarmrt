module Lambda_fn

# Seeds the fuzzer around the `fn(params){body}` lambda lookahead added to
# par_primary. Mutating these bytes stresses the balanced-paren scan in
# par_fn_lambda_ahead: unterminated parens, missing `{`, nested lambdas, `fn`
# as a plain identifier vs. a lambda introducer, EOF mid-scan, oversize param
# lists (the [16][128] cap), and the fun/fn interchange.

import Std

fun apply1(f, x) { f(x) }
fun call_fn_param(fn, x) { fn(x) }

fun main() {
    a = map(fn(x){ x + 1 }, [1, 2, 3])
    b = apply1(fn(y){ y * 2 }, 21)
    fns = [fn(){ 1 }, fn(){ 2 }, fn(z){ z }]
    m = %{ double: fn(x){ x * 2 }, id: fn(v){ v } }
    nested = fn(n){ fn(x){ x + n } }
    plain = call_fn_param(fn(q){ q }, 7)
    same = apply1(fun(w){ w - 1 }, 10)
    # `fn` used as a bare identifier (not a lambda) — must stay an ident/call
    fn = fn(x){ x }
    r = fn(99)
    # degenerate shapes the mutator will chew on:
    #   fn(
    #   fn()
    #   fn(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t){ 0 }
    #   fn(((((((((()))))))))){ 0 }
    Cron.every(200, fn(){ print("t") })
    Std.task_stream([1, 2, 3], fn(x){ x * 2 }, %{max_concurrency: 2})
}
