module Test_deep_recursion

# Phase 2c spill (docs/design/PROCESS_WEIGHT.md §3.4 rule 5): a spawned
# compiled process whose carrier fills up parks its own call, its callers
# capture themselves to the heap, and the call re-issues on an empty carrier.
# Non-tail recursion is then bounded by SW_FRAME_MAX (default 256 MB), not by
# the 128 KB carrier stack, which used to panic near 470 frames.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun depth(n) { if (n == 0) { 0 } else { 1 + depth(n - 1) } }

# Non-tail mutual recursion.
fun even_len(n) { if (n == 0) { 0 } else { 1 + odd_len(n - 1) } }
fun odd_len(n) { if (n == 0) { 0 } else { 1 + even_len(n - 1) } }

# A list built on the way back up.
fun build(n) { if (n == 0) { [] } else { [n | build(n - 1)] } }

fun lambda_depth(n) {
    f = fn(self_f, k) { if (k == 0) { 0 } else { 1 + self_f(self_f, k - 1) } }
    f(f, n)
}

fun run(parent, tag, thunk) { send(parent, {tag, thunk()}) }

fun await(tag) { receive { {t, v} when t == tag -> v after 60000 { 'timeout' } } }

fun main() {
    me = self()
    f = 0
    spawn(run(me, 'depth', fn() { depth(1000000) }))
    f = f + assert_eq("1,000,000-deep non-tail recursion", await('depth'), 1000000)
    spawn(run(me, 'mutual', fn() { even_len(200000) }))
    f = f + assert_eq("200,000-deep mutual recursion", await('mutual'), 200000)
    spawn(run(me, 'build', fn() { length(build(100000)) }))
    f = f + assert_eq("100,000-deep list build", await('build'), 100000)
    spawn(run(me, 'lambda', fn() { lambda_depth(100000) }))
    f = f + assert_eq("100,000-deep lambda recursion", await('lambda'), 100000)
    if (f == 0) { print("OK test_deep_recursion all") } else { panic("deep recursion tests failed") }
}
