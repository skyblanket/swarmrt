module Main

fun main() {
    js = "{\"name\":\"akash\",\"items\":[\"a\",\"b\",\"c\"]}"
    obj = json_decode(js)
    name = map_get(obj, "name")
    items = map_get(obj, "items")
    print(name)
    print(length(items))
}
