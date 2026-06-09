# slope_dynsup_churn.sw — memory-slope gate for dynamic-supervisor child closures.
#
# A dynamic supervisor deep-copies each child's start closure (start_arg) to the
# global heap so it can re-apply it across restarts. Before the fix, permanently
# removing a child (sup_terminate_child / no-restart exit / restart spawn-failure)
# and tearing the supervisor down freed only the list node, never the closure —
# so churning start_child/terminate_child leaked a closure graph per child.
# After: sw_spec_free_start_arg frees the closure at every permanent-removal site
# (never on a restart, where start_arg is re-passed).
#
# Bounded concurrency: one child started then immediately terminated per round, so
# ~1 child is live at a time (slab/stack flat); the only thing that can grow is a
# leaked per-child closure. RSS must be FLAT in rounds.
module Main

fun big_string(acc, n) { if (n <= 0) { acc } else { big_string(acc ++ "0123456789abcdef", n - 1) } }

# Parks so the child stays alive until sup_terminate_child removes it.
fun worker(cap) { receive { _x -> 0 after 100000 { 0 } } }

fun loop(sup, rounds, cap) {
    if (rounds <= 0) { 0 }
    else {
        c = sup_start_child(sup, {'w', fn() { worker(cap) }, 'temporary'})  # closure captures ~1 KB
        sup_terminate_child(sup, c)   # TERM_CHILD: permanent removal -> closure freed
        loop(sup, rounds - 1, cap)
    }
}

fun job_count(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 2000 } }

fun main() {
    rounds = job_count(os_args())
    cap = big_string("c", 64)        # ~1 KB captured per child closure
    sup = dyn_supervisor()
    print("slope_dynsup_churn rounds=" ++ to_string(rounds))
    loop(sup, rounds, cap)
    print("slope_dynsup_churn DONE")
    print("PROBE_OK")
    0
}
