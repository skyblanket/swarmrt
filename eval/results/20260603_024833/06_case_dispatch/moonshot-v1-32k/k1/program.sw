module Main

fun classify(value) {
    case value {
        x when typeof(x) == "int" -> "int"
        [] -> "empty_list"
        _ when typeof(value) == "list" -> f"list:{length(value)}"
        {'ok', v} -> f"ok:{v}"
        _ -> "other"
    }
}

fun main() {
    for v in [42, [], [1, 2, 3], {'ok', "hello"}, "a string"] {
        print(classify(v))
    }
}
