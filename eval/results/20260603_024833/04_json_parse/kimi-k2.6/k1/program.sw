module Main

fun main() {
    json_str = "{\"name\":\"akash\",\"items\":[\"a\",\"b\",\"c\"]}"
    decoded = json_decode(json_str)
    name = map_get(decoded, "name")
    items = map_get(decoded, "items")
    print(name)
    print(length(items))
}
