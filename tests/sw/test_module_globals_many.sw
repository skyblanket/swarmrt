module Test_module_globals_many

# Regression: module-level `let` used to be capped at 16 constants. A module
# with 40 constants, including map/list/tuple literals, must compile (swc build)
# and interpret (swc run) identically. Mirrored in tests/sw/run/.

let C1 = 1
let C2 = 2
let C3 = 3
let C4 = 4
let C5 = 5
let C6 = 6
let C7 = 7
let C8 = 8
let C9 = 9
let C10 = 10
let C11 = 11
let C12 = 12
let C13 = 13
let C14 = 14
let C15 = 15
let C16 = 16
let C17 = 17
let C18 = 18
let C19 = 19
let C20 = 20
let C21 = 21
let C22 = 22
let C23 = 23
let C24 = 24
let C25 = 25
let C26 = 26
let C27 = 27
let C28 = 28
let C29 = 29
let C30 = 30
let C31 = 31
let C32 = 32
let C33 = 33
let C34 = 34
let C35 = 35
let C36 = 36
let C37 = 37
let C38 = 38
let C39 = 39
let C40 = 40
let LIMITS = %{"lo" => 0, "hi" => 100}
let NAMES = ["a", "b", "c"]
let PAIR = {7, "seven"}

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# Reads module constants from inside a function (goes through the global table).
fun spread() { C1 + C20 + C40 }

fun main() {
    fails = 0
    fails = fails + assert_eq("first_global", C1, 1)
    fails = fails + assert_eq("last_global", C40, 40)
    fails = fails + assert_eq("globals_in_fn", spread(), 61)
    fails = fails + assert_eq("map_literal", map_get(LIMITS, "hi"), 100)
    fails = fails + assert_eq("list_literal", length(NAMES), 3)
    fails = fails + assert_eq("list_literal_elem", hd(tl(NAMES)), "b")
    fails = fails + assert_eq("tuple_literal", elem(PAIR, 1), "seven")
    if (fails == 0) { print("OK module_globals_many 7/7") ; sys_exit(0) }
    else { print("FAIL module_globals_many " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
