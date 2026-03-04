module MathTest

fun test_basic_arithmetic() {
    assert_eq(2 + 2, 4)
    assert_eq(10 - 3, 7)
    assert_eq(5 * 6, 30)
    assert_eq(10 / 2, 5)
}

fun test_comparisons() {
    assert(10 > 5)
    assert(3 < 7)
    assert(5 == 5)
    assert_ne(3, 4)
}

fun test_strings() {
    assert_eq("hello" ++ " world", "hello world")
    assert_eq(length("abc"), 3)
}

fun test_lists() {
    xs = [1, 2, 3]
    assert_eq(hd(xs), 1)
    assert_eq(length(xs), 3)
}

fun test_variables() {
    x = 42
    y = x + 8
    assert_eq(y, 50)
}

fun test_nested_expressions() {
    assert_eq((2 + 3) * 4, 20)
    assert_eq(length([1, 2, 3, 4, 5]), 5)
}

fun helper_not_a_test() {
    42
}
