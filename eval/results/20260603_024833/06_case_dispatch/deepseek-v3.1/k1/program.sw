module Main

fun classify(value) {
    case value {
        _ when typeof(value) == "int" -> "int"
        [] -> "empty_list"
        l when typeof(l) == "list" -> f"list:{length(l)}"
        {'ok', v} -> f"ok:{v}"
        _ -> "other"
    }
}

fun main() {
    values = [42, [], [1, 2, 3], {'ok', "hello"}, "a string"]
    for v in values {
        print(classify(v))
    }
}
