# slope_mutual.sw — memory-slope probe for a LONG-LIVED MUTUAL TAIL-RECURSIVE
# LOOP. The twin of slope_turn.sw with the loop split across two module
# functions (`loop -> step_turn -> loop`), the receive-loop-handler shape. The
# hop codegen runs the chain on one driver with one checkpoint floor, so the
# per-turn garbage is reclaimed exactly as the self-loop's is: the slope is flat.
#
# Usage: slope_mutual <turns>   (default 100000). Driven by scripts/gc_slope.sh.

module Main

fun big_string(acc, n) {
    if (n <= 0) { acc }
    else { big_string(acc ++ "0123456789abcdef", n - 1) }
}

# Each turn: allocate a pile of garbage (a list of maps of strings) that is NOT
# carried forward, fold it to a small int, and hand the state to the other half.
fun junk(i, n, acc) {
    if (i >= n) { acc }
    else {
        m = map_put(map_put(map_new(), 'k', big_string("", 8)), 'i', i)
        junk(i + 1, n, list_append(acc, m))
    }
}

fun loop(turns, state) {
    if (turns <= 0) { state }
    else { step_turn(turns, state) }
}

fun step_turn(turns, state) {
    g = junk(0, 40, [])             # ~per-turn garbage, dropped after this turn
    delta = length(g)
    loop(turns - 1, state + delta)  # mutual tail call — only `state` carries forward
}

fun job_count(args) {
    if (length(args) >= 2) { to_int(hd(tl(args))) } else { 100000 }
}

fun main() {
    turns = job_count(os_args())
    print("slope_mutual turns=" ++ to_string(turns))
    final = loop(turns, 0)
    print("slope_mutual DONE turns=" ++ to_string(turns) ++ " state=" ++ to_string(final))
    print("PROBE_OK")
    0
}
