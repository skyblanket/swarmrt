module Main

import Std

fun main() {
    json = "[{\"type\":\"click\",\"count\":3},{\"type\":\"view\",\"count\":10},{\"type\":\"click\",\"count\":7},{\"type\":\"signup\",\"count\":1}]"
    events = json_decode(json)
    aggregated = reduce(fun(acc, event) {
        type = map_get(event, "type")
        count = map_get(event, "count")
        current_count = map_get(acc, type, 0)
        map_put(acc, type, current_count + count)
    }, events, %{})
    sorted_types = Std.sort(map_keys(aggregated))
    Std.each(sorted_types, fun(type) {
        total = map_get(aggregated, type)
        print(f"{type}: {total}")
    })
}
