module Main

fun classify(value) {
    case value {
        _ when typeof(value) == "int" -> "int"
        [] -> "empty_list"
        _ when typeof(value) == "list" -> f"list:{length(value)}"
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
