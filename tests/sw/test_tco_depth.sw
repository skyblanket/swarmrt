module Test_tco_depth

# Compiled-path gate: a self-tail-call must run at UNBOUNDED depth on a
# flat stack (codegen rewrites it into the _tail loop). 2M iterations
# would devour ~gigabytes of stack frames without TCO; with it, this is
# a plain loop inside one 128KB fiber. The interpreter is exempt by
# design (C-stack tree-walker with a stack guard, no TCO) — which is
# why this lives in the compiled suite, not tests/sw/conform/.
#
# Also pins the loop-backedge yield: sw_check_reds()/sw_yield() at the
# goto _tail backedge must not corrupt locals across a descheduling.

fun count(n, i) {
    if (i >= n) { i }
    else { count(n, i + 1) }
}

fun sum_to(n, i, acc) {
    if (i > n) { acc }
    else { sum_to(n, i + 1, acc + i) }
}

fun main() {
    failures = 0

    d = count(2000000, 0)
    failures = failures + (if (d == 2000000) { print("PASS tco_depth_2m") ; 0 }
                           else { print(f"FAIL tco_depth_2m: got {d}") ; 1 })

    s = sum_to(100000, 1, 0)
    failures = failures + (if (s == 5000050000) { print("PASS tco_accumulator") ; 0 }
                           else { print(f"FAIL tco_accumulator: got {s}") ; 1 })

    if (failures == 0) { print("OK test_tco_depth 2/2") ; sys_exit(0) }
    else { print(f"FAIL test_tco_depth: {failures} failed") ; sys_exit(1) }
}
