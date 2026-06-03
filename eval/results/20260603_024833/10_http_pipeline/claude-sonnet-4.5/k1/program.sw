module Main

import Std

fun main() {
    input = "[{\"type\":\"click\",\"count\":3},{\"type\":\"view\",\"count\":10},{\"type\":\"click\",\"count\":7},{\"type\":\"signup\",\"count\":1}]"
    events = json_decode(input)
    
    grouped = Std.group_by(events, fun(e) { map_get(e, "type") })
    keys = Std.sort(map_keys(grouped))
    
    Std.each(keys, fun(type) {
        items = map_get(grouped, type)
        total = Std.sum(map(items, fun(e) { map_get(e, "count") }))
        print(f"{type}: {total}")
    })
}
