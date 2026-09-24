module Test_preemption

# Regression: a long NON-tail computation must not starve other processes on
# its scheduler. Only self-tail-call backedges used to yield, so on one
# scheduler a heartbeat process never ran while fib(30) was computing. Every
# compiled call now counts a reduction and yields when the slice is used up.
# Re-runs itself with SW_SCHEDULERS=1, where the old behaviour was 0 ticks.

fun fib(n) { if (n < 2) { n } else { fib(n - 1) + fib(n - 2) } }

fun beat(parent, n) {
    receive { 'stop' -> send(parent, {'beats', n}) after 5 -> beat(parent, n + 1) }
}

fun probe() {
    me = self()
    hb = spawn(beat(me, 0))
    sleep(20)
    f = fib(30)
    send(hb, 'stop')
    receive { {'beats', n} -> print(f"{f} {n}") after 5000 -> print("no-report") }
}

fun main() {
    args = os_args()
    if (length(args) > 1) { probe() }
    else {
        out = string_trim(elem(shell("SW_SCHEDULERS=1 " ++ hd(args) ++ " probe"), 1))
        parts = string_split(out, " ")
        ticks = if (length(parts) == 2) { to_int(hd(tl(parts))) } else { 0 }
        if (hd(parts) == "832040" && ticks >= 3) {
            print("PASS heartbeat_ran_during_fib") ; print("OK preemption 1/1") ; sys_exit(0)
        } else {
            print("FAIL preemption: probe said '" ++ out ++ "' (expected fib=832040 and >= 3 heartbeat ticks)")
            sys_exit(1)
        }
    }
}
