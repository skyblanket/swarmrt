module Main

fun main() {
    json_str = "{\"name\":\"akash\",\"items\":[\"a\",\"b\",\"c\"]}"
    data = json_decode(json_str)
    name = map_get(data, "name")
    items = map_get(data, "items")
    print(name)
    print(length(items))
}
