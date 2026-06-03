module Main

fun classify(value) {
    case value {
        [] -> "empty_list"
        [h | t] -> f"list:{length(value)}"
        _ when typeof(value) == "int" -> "int"
        {'ok', v} -> f"ok:{v}"
        _ -> "other"
    }
}

fun main() {
    print(classify(42))
    print(classify([]))
    print(classify([1, 2, 3]))
    print(classify({'ok', "hello"}))
    print(classify("a string"))
}
