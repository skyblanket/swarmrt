# slope_sup_kill.sw — memory-slope gate for SUPERVISOR KILL-TEARDOWN (audit w2bqfncq7 P1-A).
#
# exit_proc(sup,'killed') is the ONLY teardown verb sw exposes for a bare
# supervise()/dyn_supervisor() pid (SW_TAG_STOP is internal). A killed supervisor
# is parked in sw_receive_any and the scheduler tears it down WITHOUT resuming the
# fiber (kill_flag checked before swap-in), so the post-receive-loop teardown
# (sup_free_child_masters / dynsup_kill_all + free(st)) NEVER ran — every child
# MASTER closure + dynsup list nodes + (heap) st leaked, unboundedly, on the
# dominant real-world teardown path. The fix moves teardown into the supervisor's
# own on_destroy hook (process_destroy always runs, incl. the kill path).
#
# Each round kills BOTH a dynamic and a static supervisor (each with a child whose
# closure captures ~64 KB — large so a per-kill master leak is RSS-visible; a ~1 KB
# capture is masked by slope noise). Bounded concurrency (reaped before next round)
# => RSS must be FLAT in rounds. Pre-fix this grows ~128 KB/round.
module Main

fun big_string(acc, n) { if (n <= 0) { acc } else { big_string(acc ++ "0123456789abcdef", n - 1) } }
fun w(cap) { receive { _x -> 0 after 100000 { 0 } } }   # parks until killed

fun loop(rounds, cap) {
    if (rounds <= 0) { 0 }
    else {
        # Dynamic supervisor: start a child (master stored in a heap node), then kill.
        d = dyn_supervisor(100, 1)
        sup_start_child(d, {'w', fn() { w(cap) }, 'permanent'})
        exit_proc(d, 'killed')        # dynsup kill -> destructor frees master+node+st
        # Static supervisor: one child, then kill.
        s = supervise('one_for_one', [{'w', fn() { w(cap) }, 'permanent'}])
        sleep(1)                       # let supervisor_entry copy specs into children[]
        exit_proc(s, 'killed')        # static kill -> destructor frees masters+st
        sleep(2)                       # reap both supervisors + their children
        loop(rounds - 1, cap)
    }
}

fun job_count(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 800 } }

fun main() {
    rounds = job_count(os_args())
    cap = big_string("c", 4096)        # ~64 KB captured per child closure
    print("slope_sup_kill rounds=" ++ to_string(rounds))
    loop(rounds, cap)
    print("slope_sup_kill DONE")
    print("PROBE_OK")
    0
}
