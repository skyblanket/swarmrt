module Main

import Std

fun main() {
    json = "[{\"type\":\"click\",\"count\":3},{\"type\":\"view\",\"count\":10},{\"type\":\"click\",\"count\":7},{\"type\":\"signup\",\"count\":1}]"
    events = json_decode(json)
    grouped = Std.group_by(events, fun(ev) { map_get(ev, "type") })
    types = Std.sort(map_keys(grouped))
    Std.each(types, fun(t) {
        evs = map_get(grouped, t)
        total = Std.sum([map_get(e, "count") for e in evs])
        print(f"{t}: {total}")
    })
}
