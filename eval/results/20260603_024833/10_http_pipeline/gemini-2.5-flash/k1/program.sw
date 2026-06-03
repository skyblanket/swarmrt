module Main

import Std

fun main() {
    json_str = "[{\"type\":\"click\",\"count\":3},{\"type\":\"view\",\"count\":10},{\"type\":\"click\",\"count\":7},{\"type\":\"signup\",\"count\":1}]"
    events = json_decode(json_str)

    grouped_data = reduce(fun(acc, event) {
        type = map_get(event, "type")
        count = map_get(event, "count")
        current_sum = map_get(acc, type, 0)
        map_put(acc, type, current_sum + count)
    }, events, %{})

    sorted_types = Std.sort(map_keys(grouped_data))

    Std.each(sorted_types, fun(type) {
        total = map_get(grouped_data, type)
        print(f"{type}: {total}")
    })
}
