module Conform_panic

# Conformance: a builtin panic (hd on []) must behave the SAME in the
# interpreter and compiled: NOT caught by try/catch, message printed,
# exit code 1. The lines before it prove normal output flushes first.
# (This file is expected to exit non-zero in BOTH paths; the runner
# compares stdout + exit codes, not success.)

fun main() {
    print("before panic")
    r = try { hd([]) } catch e { "must-not-catch" }
    print(f"unreachable: {r}")
}
