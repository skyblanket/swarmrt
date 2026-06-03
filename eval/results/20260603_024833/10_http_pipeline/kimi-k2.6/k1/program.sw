module Main

import Std

fun main() {
    json = "[{\"type\":\"click\",\"count\":3},{\"type\":\"view\",\"count\":10},{\"type\":\"click\",\"count\":7},{\"type\":\"signup\",\"count\":1}]"
    events = json_decode(json)
    by_type = Std.group_by(events, fun(e) { map_get(e, "type") })
    keys = Std.sort(map_keys(by_type))
    Std.each(keys, fun(k) {
        total = map_get(by_type, k) |> map(fun(e) { map_get(e, "count") }) |> Std.sum
        print(f"{k}: {total}")
    })
}
