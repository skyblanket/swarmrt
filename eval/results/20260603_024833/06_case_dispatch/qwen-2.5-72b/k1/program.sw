module Main

import Std

fun classify(value) {
    case value {
        _ when typeof(value) == "int"      -> "int"
        []                                -> "empty_list"
        [h | t]                           -> f"list:{length([h | t])}"
        {'ok', v}                         -> f"ok:{v}"
        _                                 -> "other"
    }
}

fun main() {
    test_values = [42, [], [1, 2, 3], {'ok', "hello"}, "a string"]
    map(test_values, fun(v) { print(classify(v)) })
}
