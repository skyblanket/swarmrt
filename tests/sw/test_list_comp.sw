module Test_list_comp

# Test list comprehension syntax: [expr for x in list] and [expr for x in list when guard]
# Follows the standard compile-path harness pattern (3-arg assert_eq + main).

fun check_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun test_double_all() {
    nums = [1, 2, 3, 4, 5]
    doubled = [x * 2 for x in nums]
    check_eq("double_all", doubled, [2, 4, 6, 8, 10])
}

fun test_empty_source() {
    result = [x * 2 for x in []]
    check_eq("empty_source", result, [])
}

fun test_literal_iter() {
    result = [x + 10 for x in [1, 2, 3]]
    check_eq("literal_iter", result, [11, 12, 13])
}

fun test_guard_even() {
    nums = [1, 2, 3, 4, 5, 6]
    evens = [x for x in nums when x % 2 == 0]
    check_eq("guard_even", evens, [2, 4, 6])
}

fun test_guard_positive() {
    nums = [-3, -1, 0, 2, 5]
    pos = [x for x in nums when x > 0]
    check_eq("guard_positive", pos, [2, 5])
}

fun test_guard_transform() {
    nums = [1, 2, 3, 4, 5]
    doubled_odds = [x * 2 for x in nums when x % 2 != 0]
    check_eq("guard_transform", doubled_odds, [2, 6, 10])
}

fun test_guard_all_fail() {
    nums = [1, 3, 5]
    evens = [x for x in nums when x % 2 == 0]
    check_eq("guard_all_fail", evens, [])
}

fun test_nested_expr() {
    nums = [1, 2, 3]
    result = [(x * x) + 1 for x in nums]
    check_eq("nested_expr", result, [2, 5, 10])
}

fun main() {
    fails = 0
    fails = fails + test_double_all()
    fails = fails + test_empty_source()
    fails = fails + test_literal_iter()
    fails = fails + test_guard_even()
    fails = fails + test_guard_positive()
    fails = fails + test_guard_transform()
    fails = fails + test_guard_all_fail()
    fails = fails + test_nested_expr()
    if (fails == 0) { print("OK list_comp 8/8") ; sys_exit(0) }
    else { print("FAIL list_comp " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
