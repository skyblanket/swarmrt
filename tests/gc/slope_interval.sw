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
        # No yield between interval() and exit_proc(): exercises BOTH the
        # cancel-while-parked path AND the PRE-TRAMPOLINE kill (child killed before
        # its entry fn ever runs — 100% under SW_SCHEDULERS=1). Both must reclaim the
        # closure (on_destroy armed pre-runnable by sw_spawn_dtor). A long period
        # means it never ticks; the leak (c+c->fn) is per-spawned-interval regardless.
        p = interval(100000, fn() { cap })
        exit_proc(p, 'killed')            # cancel immediately (no intervening yield)
        sleep(1)                          # reaped before the next round (bounded concurrency)
        loop(rounds - 1, cap)
    }
}

fun job_count(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 1500 } }

fun main() {
    rounds = job_count(os_args())
    cap = big_string("c", 4096)            # ~64 KB captured per interval closure — large so a
                                           # per-cancel closure leak is RSS-visible (a ~1 KB
                                           # capture is masked by slope noise; audit w2bqfncq7)
    print("slope_interval rounds=" ++ to_string(rounds))
    loop(rounds, cap)
    print("slope_interval DONE")
    print("PROBE_OK")
    0
}
