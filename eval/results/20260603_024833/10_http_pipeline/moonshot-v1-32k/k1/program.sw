module Main

import Std
import Json

fun main() {
    events_json = "[{\"type\":\"click\",\"count\":3},{\"type\":\"view\",\"count\":10},{\"type\":\"click\",\"count\":7},{\"type\":\"signup\",\"count\":1}]"
    events = Json.decode(events_json)

    if (events == 'nil') {
        panic("bad json")
    }

    grouped = Std.group_by(events, fun(event) { Json.get(event, "type") })
    sorted_keys = Std.sort(map_keys(grouped))
    total_per_type = [Std.string_pad_left(key, 6) ++ ": " ++ to_string(Std.sum(map_get(grouped, key), fun(ev) { Json.get(ev, "count") })) for key in sorted_keys]

    Std.each(total_per_type, fun(line) { print(line) })
}
