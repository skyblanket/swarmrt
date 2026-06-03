module Main

import Std

fun main() {
    json_string = f'{{"name":"akash","items":["a","b","c"]}}'
    decoded = json_decode(json_string)
    
    name = map_get(decoded, "name")
    print(name)
    
    items = map_get(decoded, "items")
    count = length(items)
    print(to_string(count))
}
