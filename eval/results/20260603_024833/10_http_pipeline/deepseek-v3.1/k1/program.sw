module Main

import Std

fun main() {
    json_str = "[{\"type\":\"click\",\"count\":3},{\"type\":\"view\",\"count\":10},{\"type\":\"click\",\"count\":7},{\"type\":\"signup\",\"count\":1}]"
    
    events = case json_decode(json_str) {
        {'ok', decoded} -> decoded
        {'error', _}    -> []
    }
    
    grouped = Std.group_by(events, fun(event) {
        map_get(event, "type")
    })
    
    aggregates = {}
    
    # Sum counts per type
    for k in map_keys(grouped) {
        bucket = map_get(grouped, k)
        total = Std.sum([map_get(e, "count") for e in bucket])
        aggregates = map_put(aggregates, k, total)
    }
    
    # Print sorted by type
    types = Std.sort(map_keys(aggregates))
    for t in types {
        total = map_get(aggregates, t)
        print(f"{t}: {total}")
    }
}
