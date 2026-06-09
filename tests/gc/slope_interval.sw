# slope_interval.sw — memory-slope + cancellability gate for interval() timers.
#
# interval(ms, fn) spawns a process that re-applies a global-heap deep copy of fn
# forever. Before the fix that loop used raw usleep + an infinite C for(;;): it
# never yielded, so a killed interval was UNINTERRUPTIBLE — kill_flag was never
# observed, process_destroy never ran, and the closure (c + c->fn) leaked forever.
# After: the loop waits via the yielding, kill-aware sw_receive_any, observes the
# kill, unwinds, and frees its own closure.
#
# Deterministic + bounded concurrency: each round starts ONE interval (closure
# captures ~1 KB), lets it tick, cancels it with exit_proc, and waits for it to be
# reaped before the next round. So ~1 interval process is live at a time (slab/
# stack memory flat); the only thing that could grow is a leaked closure.
#
# DOUBLES AS A REGRESSION TEST FOR CANCELLABILITY: pre-fix, exit_proc cannot stop
# the spinning interval at all, so the probe HANGS (never prints PROBE_OK) and the
# gate fails on the missing PROBE_OK / timeout. Post-fix it terminates with flat RSS.
module Main

fun big_string(acc, n) { if (n <= 0) { acc } else { big_string(acc ++ "0123456789abcdef", n - 1) } }

fun loop(rounds, cap) {
    if (rounds <= 0) { 0 }
    else {
        # Long period: the interval parks in sw_receive_any almost immediately and
        # never ticks; exit_proc then wakes the parked receive (mailbox_wake) so it
        # unwinds and frees promptly — no need to wait for a tick. The leak (c+c->fn)
        # is per-spawned-interval regardless of whether it fired.
        p = interval(100000, fn() { cap })
        sleep(1)                          # let it reach sw_receive_any and park
        exit_proc(p, 'killed')            # cancel — wakes the parked receive -> free
        sleep(1)                          # reaped before the next round (bounded concurrency)
        loop(rounds - 1, cap)
    }
}

fun job_count(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 1500 } }

fun main() {
    rounds = job_count(os_args())
    cap = big_string("c", 64)            # ~1 KB captured per interval closure
    print("slope_interval rounds=" ++ to_string(rounds))
    loop(rounds, cap)
    print("slope_interval DONE")
    print("PROBE_OK")
    0
}
