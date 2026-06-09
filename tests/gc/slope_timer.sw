# slope_timer.sw — memory-slope probe for one-shot timer closures (delay).
#
# delay(ms, fn) deep-copies fn to the global heap (it fires after the caller may
# have exited) and spawns a timer process. Before the fix the timer process freed
# its struct but NOT the deep-copied closure, so every delay() that fired leaked a
# closure graph — repeated use grew RSS without bound. After: the one-shot timer
# frees the whole closure (captures included) once its single apply returns.
#
# Deterministic AND bounded-concurrency: each round fires ONE delay whose closure
# captures a ~1 KB string and SENDs it back, then waits for that reply before the
# next round. So at most one timer process is live at a time — process/stack
# memory stays flat (slab reuse), isolating the CLOSURE leak as the only thing
# that could grow. RSS must be FLAT in rounds; pre-fix it grows ~1 KB/round.
module Main

fun big_string(acc, n) { if (n <= 0) { acc } else { big_string(acc ++ "0123456789abcdef", n - 1) } }

fun one(parent) { receive { _x -> 0 after 10000 { 0 } } }

fun spin(parent, rounds) {
    if (rounds <= 0) { 0 }
    else {
        cap = big_string("c", 64)                 # ~1 KB captured by the closure
        delay(0, fn() { send(parent, cap) })       # one-shot; fires then frees its closure
        one(parent)                                # wait for THIS delay to fire+exit before the next
        spin(parent, rounds - 1)
    }
}

fun job_count(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 3000 } }

fun main() {
    rounds = job_count(os_args())
    me = self()
    print("slope_timer rounds=" ++ to_string(rounds))
    spin(me, rounds)
    print("slope_timer DONE")
    print("PROBE_OK")
    0
}
