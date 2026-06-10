# alloc_fault.sw — workload for the Phase 2.5 allocation-failure sweep.
#
# Driven by `make alloc-fault`: built with -DSW_ALLOC_FAULT + ASAN and run
# repeatedly with SW_FAIL_ALLOC_AT=1,2,3,... so the Nth value/region
# allocation fails exactly once. Exercises the ownership-transfer sites
# whose failure cleanup must neither leak nor double-free:
#   - spawn (spawn-arg region adopted by child, or freed if spawn fails)
#   - send (message payload deep-copied into an adopted region)
#   - supervise + child (per-incarnation closure copies)
#   - ETS put (value copied into the table)
#
# A given fail point yields one of: clean completion, or a loud OOM/spawn
# panic (exit 1) — BOTH acceptable. The gate fails ONLY on an ASAN report
# (use-after-free / double-free / heap-buffer-overflow) — i.e. a fault
# that corrupted memory instead of unwinding cleanly.
module Main

fun worker(parent) {
    receive {
        {'job', payload, from} -> send(from, {'done', payload}) ; worker(parent)
        'stop' -> 'ok'
        after 3000 { 'ok' }
    }
}

fun fan(n, me) {
    if (n <= 0) { 'ok' }
    else {
        w = spawn(worker(me))
        send(w, {'job', {n, "payload-data-here", [1, 2, 3]}, me})
        receive { {'done', _} -> 0 after 2000 { 0 } }
        send(w, 'stop')
        fan(n - 1, me)
    }
}

fun main() {
    # ETS churn
    t = ets_new()
    ets_put(t, 'k', {1, "v", [9, 8, 7]})
    ets_put(t, 'k', {2, "w", [6, 5]})

    # supervised child
    s = supervise('one_for_one', [{'sup_w', fun() { worker(self()) }, 'permanent'}])
    sleep(5)

    # fan-out spawn + message round-trips (the main ownership-transfer churn)
    fan(20, self())

    exit_proc(s, 'killed')
    print("alloc_fault DONE")
    0
}
