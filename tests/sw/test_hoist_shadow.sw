module Test_hoist_shadow

# Phase 2b (docs/design/PROCESS_WEIGHT.md): the codegen hoists every C local
# to function scope, and a binding that shadows a visible one gets its own C
# name. If a shadowing binding shared the outer one's slot, the outer value
# would be clobbered once the inner scope closed. Each test rebinds a name
# in an inner scope and then reads the outer binding again.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun nested_case(x) {
    r = case {x, 2} {
        {1, x} -> x * 10
        {_, x} -> x * 100
    }
    {r, x}
}

# Only the loop's own result is checked: after the loop the interpreter
# leaves `x` at the last element while compiled code keeps the outer `x`
# (a parity gap that predates hoisting).
fun for_var() {
    x = 5
    acc = 0
    for x in [1, 2, 3] { acc = acc + x * 10 }
    acc
}

fun comp_var() {
    x = 7
    ys = [x * 2 for x in [1, 2, 3]]
    {ys, x}
}

fun two_trys() {
    e = "outer"
    a = try { error("one") } catch e { e }
    b = try { error("two") } catch e { e }
    {a, b, e}
}

fun lambda_shadow() {
    k = 3
    f = fn(k) { k + 1 }
    g = fn() { k * 2 }
    {f(10), g(), k}
}

fun recv_shadow() {
    v = 'outer'
    send(self(), {'msg', 'inner'})
    got = receive { {'msg', v} -> v after 1000 { 'timeout' } }
    {got, v}
}

fun main() {
    f = 0
    f = f + assert_eq("case arm shadows a parameter", nested_case(1), {20, 1})
    f = f + assert_eq("case fallback arm shadows a parameter", nested_case(5), {200, 5})
    f = f + assert_eq("for variable shadows a local", for_var(), 60)
    f = f + assert_eq("comprehension variable shadows a local", comp_var(), {[2, 4, 6], 7})
    f = f + assert_eq("catch variable shadows a local", two_trys(), {"one", "two", "outer"})
    f = f + assert_eq("lambda parameter shadows a capture", lambda_shadow(), {11, 6, 3})
    f = f + assert_eq("receive binding shadows a local", recv_shadow(), {'inner', 'outer'})
    if (f == 0) { print("OK test_hoist_shadow all") } else { panic("hoist shadow tests failed") }
}
