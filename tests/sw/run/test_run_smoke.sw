# test_run_smoke.sw — end-to-end smoke test for `swc run` (the interpreter
# run path). Exercised by tests/sw/run_tests.sh via `swc run`, which:
#   - parses this file
#   - resolves `import Math` from the bundled lib/
#   - merges modules and calls main() in the tree-walking interpreter
#
# Self-checking: prints "RUN_SMOKE_OK" and exits 0 only if every check
# passes; otherwise prints the failure and exits 1.

module Test_run_smoke
import Math

fun chk(name, actual, expected) {
    if (actual == expected) { 0 }
    else {
        print("RUN_SMOKE_FAIL " ++ name ++ ": expected " ++
              to_string(expected) ++ ", got " ++ to_string(actual))
        1
    }
}

fun main() {
    fails = 0
    # imported lib resolves and runs under the interpreter
    fails = fails + chk("math_sqrt", Math.sqrt(81), 9.0)
    fails = fails + chk("math_clamp", Math.clamp(15, 0, 10), 10)
    fails = fails + chk("math_pi", Math.pi(), 3.141592653589793)
    # functional primitives work in the interpreter run path
    fails = fails + chk("reduce", reduce(fun(a, x) { a + x }, [1, 2, 3], 0), 6)
    # dogfood builtins work under `swc run`
    fails = fails + chk("ord", ord("A"), 65)
    fails = fails + chk("byte_size", byte_size(bytes_from_ints([1, 2, 3])), 3)
    if (fails == 0) { print("RUN_SMOKE_OK") ; sys_exit(0) }
    else { sys_exit(1) }
}
