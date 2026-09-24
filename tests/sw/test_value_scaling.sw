module Test_value_scaling

# Regression: idiomatic value code must scale linearly, not quadratically or
# exponentially. Each probe below took minutes to hours (or ran out of
# memory) before the memory-model fixes, so the suite's per-test timeout is
# the gate; the assertions check the answers.
#   - hd/tl recursion over a list: tl() copied the whole list (O(n^2)).
#   - list_append / [x | acc] accumulators: every step copied (O(n^2)).
#   - a loop carrying a value that shares substructure: the turn checkpoint
#     deep-copied it as a tree, 2^k nodes for k shared levels (OOM).
#   - fib(25): every intermediate int and comparison result was a fresh
#     allocation never reclaimed inside the call (1.2 GB for fib(30)).

import Std

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun sum(lst, acc) { if (length(lst) == 0) { acc } else { sum(tl(lst), acc + hd(lst)) } }
fun build(n, acc) { if (n == 0) { acc } else { build(n - 1, [n | acc]) } }
fun grow(i, n, acc) { if (i == n) { acc } else { grow(i + 1, n, list_append(acc, i)) } }

fun junk(acc, n) { if (n <= 0) { acc } else { junk(acc ++ "0123456789abcdef", n - 1) } }
fun shared(t, k) {
    j = junk("", 200)
    if (k == 0) { t } else { shared({t, t}, k - 1) }
}
fun depth(t, d) { if (typeof(t) == "tuple") { depth(elem(t, 0), d + 1) } else { d } }

fun fib(n) { if (n < 2) { n } else { fib(n - 1) + fib(n - 2) } }

fun main() {
    f = 0
    f = f + assert_eq("cons_build_sum", sum(build(200000, []), 0), 20000100000)
    f = f + assert_eq("append_build_len", length(grow(0, 200000, [])), 200000)
    f = f + assert_eq("std_range_len", length(Std.range(0, 200000)), 200000)
    f = f + assert_eq("shared_depth", depth(shared(1, 28), 0), 28)
    f = f + assert_eq("fib25", fib(25), 75025)
    if (f == 0) { print("OK value_scaling 5/5") ; sys_exit(0) }
    else { print("FAIL value_scaling") ; sys_exit(1) }
}
