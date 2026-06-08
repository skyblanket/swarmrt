# slope_turn.sw — memory-slope probe for a LONG-LIVED TAIL-RECURSIVE LOOP
# (Ownership v2 turn-checkpoint). The canonical agent shape: a function that
# loops forever, building per-turn garbage (decoded data, temp lists/strings)
# and carrying a small state forward. Under GC v1 the process arena is freed only
# at process exit, so every turn's garbage accumulates → RSS grows with turns.
# After Ownership v2 (turn-checkpoint: copy the recursion args into a fresh arena
# and bulk-free the old once it crosses the reset threshold) the slope is flat.
#
# Usage: slope_turn <turns>   (default 100000). Driven by scripts/gc_slope.sh.

module Main

fun big_string(acc, n) {
    if (n <= 0) { acc }
    else { big_string(acc ++ "0123456789abcdef", n - 1) }
}

# Each turn: allocate a pile of garbage (a list of maps of strings) that is NOT
# carried forward, fold it to a small int, and recurse with a small state.
fun junk(i, n, acc) {
    if (i >= n) { acc }
    else {
        m = map_put(map_put(map_new(), 'k', big_string("", 8)), 'i', i)
        junk(i + 1, n, list_append(acc, m))
    }
}

fun loop(turns, state) {
    if (turns <= 0) { state }
    else {
        g = junk(0, 40, [])             # ~per-turn garbage, dropped after this turn
        delta = length(g)
        loop(turns - 1, state + delta)  # tail call — only `state` carries forward
    }
}

fun job_count(args) {
    if (length(args) >= 2) { to_int(hd(tl(args))) } else { 100000 }
}

fun main() {
    turns = job_count(os_args())
    print("slope_turn turns=" ++ to_string(turns))
    final = loop(turns, 0)
    print("slope_turn DONE turns=" ++ to_string(turns) ++ " state=" ++ to_string(final))
    print("PROBE_OK")
    0
}
