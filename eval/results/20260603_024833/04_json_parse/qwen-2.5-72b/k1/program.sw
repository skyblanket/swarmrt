module Main

fun main() {
    json = '{"name":"akash","items":["a","b","c"]}'
    decoded = json_decode(json)
    name = map_get(decoded, "name")
    items = map_get(decoded, "items")
    print(name)
    print(length(items))
}
