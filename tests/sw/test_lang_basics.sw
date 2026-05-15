module Test_lang_basics

# Tiny in-file harness — keeps tests self-contained and dependency-free.
# Each test prints `PASS <name>` or `FAIL <name>: <msg>`. main() exits
# non-zero if anything failed so the driver can roll up the totals.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun assert_true(name, cond) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name ++ ": condition false") ; 1 }
}

# % modulo (FIXED 2026-05-15)
fun test_modulo() { assert_eq("modulo", 17 % 5, 2) }

# Map literal disambiguation: `%{` is one token, doesn't conflict with %
fun test_map_literal() {
    m = %{a: 1, b: 2}
    assert_eq("map_literal", map_get(m, 'a'), 1)
}

# `;` separator inside if-branch (FIXED 2026-05-15)
fun test_semi_in_if_branch() {
    x = 1
    a = if (x == 1) { y = 10 ; y * 2 } else { 0 }
    assert_eq("semi_in_if_branch", a, 20)
}

# Variable scope shadowing across if/else branches (FIXED 2026-05-15)
fun test_scope_shadowing() {
    pick_first = 'true'
    r = if (pick_first == 'true') { wrapped = "first" ; wrapped }
         else { wrapped = "second" ; wrapped }
    assert_eq("scope_shadowing", r, "first")
}

# C-reserved-word identifiers (FIXED 2026-05-15)
fun test_c_reserved_idents() {
    inline = "hello"
    static = " world"
    register = inline ++ static
    assert_eq("c_reserved_idents", register, "hello world")
}

fun main() {
    fails = 0
    fails = fails + test_modulo()
    fails = fails + test_map_literal()
    fails = fails + test_semi_in_if_branch()
    fails = fails + test_scope_shadowing()
    fails = fails + test_c_reserved_idents()
    if (fails == 0) { print("OK lang_basics 5/5") ; sys_exit(0) }
    else { print("FAIL lang_basics " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
