# sup_restart_repro.sw — double-free / UAF tripwire for supervisor child closures
# under the master + per-incarnation-copy ownership model.
#
# The supervisor keeps a MASTER closure and hands each (re)started child a fresh
# COPY (freed by the child in process_destroy — crash-safe, since a panic
# sw_context_swaps to the scheduler and never returns through the entry fn). The
# master is freed ONLY at permanent removal / teardown, NEVER on a restart. This
# exercises that split: a PERMANENT child that crashes repeatedly is restarted
# (new copy each incarnation; each dead incarnation frees its own copy), then the
# whole supervisor is torn down (master freed once). Under ASAN + -DSW_ARENA_POISON
# a master freed-on-restart, or a double-freed copy, faults. Clean => the split
# holds across crash + restart + teardown.
module Main

fun big_string(acc, n) { if (n <= 0) { acc } else { big_string(acc ++ "0123456789abcdef", n - 1) } }

# Crashes shortly after starting, so the supervisor restarts it.
fun crasher(cap) {
    sleep(2)
    panic("crasher down: " ++ to_string(length(cap)))
}

fun outer(rounds, cap) {
    if (rounds <= 0) { 0 }
    else {
        d = dyn_supervisor(100, 1)             # high restart limit -> keeps restarting
        sup_start_child(d, {'crasher', fn() { crasher(cap) }, 'permanent'})
        sleep(14)                              # several crash + restart cycles
        exit_proc(d, 'killed')                 # teardown -> free master once
        sleep(3)                               # reap before next round
        outer(rounds - 1, cap)
    }
}

fun job_count(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 40 } }

fun main() {
    rounds = job_count(os_args())
    cap = big_string("x", 24)
    print("sup_restart_repro rounds=" ++ to_string(rounds))
    outer(rounds, cap)
    print("sup_restart_repro DONE")
    print("PROBE_OK")
    0
}
