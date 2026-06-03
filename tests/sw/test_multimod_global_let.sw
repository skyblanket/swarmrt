module Test_multimod_global_let

# Regression: a top-level `let X = ...` in an IMPORTED module must be
# initialized before the entry module uses it. Before the fix this
# SIGSEGV'd because _main_entry only initialized the entry module's
# globals — the imported module's `let` slots stayed NULL.

import MmGlobalLet

let ENTRY_TAG = "entry"

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun main() {
    fails = 0
    # Imported module's string `let` — used to SIGSEGV in `++`.
    fails = fails + assert_eq("imported_string_let", MmGlobalLet.base(), "v")
    # Imported module's int `let`.
    fails = fails + assert_eq("imported_int_let", MmGlobalLet.count(), 7)
    # Imported `let` consumed inside the imported module's own fn.
    fails = fails + assert_eq("imported_let_in_concat", MmGlobalLet.greet("hi"), "v:hi")
    # Entry module's own `let` still initializes (regression guard).
    fails = fails + assert_eq("entry_own_let", ENTRY_TAG, "entry")
    if (fails == 0) { print("OK multimod_global_let 4/4") ; sys_exit(0) }
    else { print("FAIL multimod_global_let " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
