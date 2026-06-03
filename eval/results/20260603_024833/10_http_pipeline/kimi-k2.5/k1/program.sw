module Main

import Std

fun main() {
    json_str = "[{\"type\":\"click\",\"count\":3},{\"type\":\"view\",\"count\":10},{\"type\":\"click\",\"count\":7},{\"type\":\"signup\",\"count\":1}]"
    events = json_decode(json_str)
    grouped = Std.group_by(events, fun(e) { map_get(e, "type") })
    types = Std.sort(map_keys(grouped))
    for t in types {
        total = Std.sum(map(map_get(grouped, t), fun(e) { map_get(e, "count") }))
        print(f"{t}: {total}")
    }
}
