# soak_mixed.sw — Phase-2.6 long-duration mixed-workload soak.
#
# Exercises the runtime's fault-tolerance, GC, scheduler, and ETS under
# realistic concurrent pressure for as long as SOAK_SECONDS allows.
# Design: a fixed number of "arena" workers each running a tight loop;
# any unexpected exit prints a failure reason and the whole program exits 1.
# On clean completion it prints SOAK_OK + a metric summary.
#
# Workload mix per iteration:
#   (a) Supervisor crash/restart storm — permanent children that panic on
#       every Nth call; the supervisor restarts them. Exercises the
#       master+copy closure machinery + on_destroy reclamation.
#   (b) ETS churn — high-throughput put/get/delete on a small live-set.
#       Exercises free-on-replace + copy-out-on-read under concurrency.
#   (c) timer cancel loop — start intervals then cancel them immediately;
#       exercises the pre-trampoline kill + on_destroy hook.
#   (d) pmap message ring — large value messages through pmap workers,
#       exercises message-region adoption + slope.
#   (e) spawn storm — high-process-count spawn/exit to stress slot reuse,
#       generation counters, arena reclamation.
#
# Safe to kill at any time — processes are isolated.

module Main

fun big(acc, n) {
    if (n <= 0) { acc }
    else { big(acc ++ "0123456789abcdef", n - 1) }
}

# ── (a) Supervisor crash/restart storm ──────────────────────────────────

fun crasher(cap) {
    sleep(2)
    panic("soak crash: " ++ to_string(length(cap)))
}

fun sup_storm(rounds, cap) {
    if (rounds <= 0) { 'sup_done' }
    else {
        d = dyn_supervisor(1000, 1)
        sup_start_child(d, {'c', fn() { crasher(cap) }, 'permanent'})
        sleep(10)
        exit_proc(d, 'killed')
        sleep(3)
        sup_storm(rounds - 1, cap)
    }
}

# ── (b) ETS churn ────────────────────────────────────────────────────────

fun ets_churn(tid, rounds, cap) {
    if (rounds <= 0) { 'ets_done' }
    else {
        k = rounds % 32
        ets_put(tid, k, {cap, rounds})
        _v = ets_get(tid, k)
        if (rounds % 5 == 0) { ets_delete(tid, (rounds + 7) % 32) }
        ets_churn(tid, rounds - 1, cap)
    }
}

# ── (c) Timer cancel loop ────────────────────────────────────────────────

fun timer_cancel_loop(rounds, cap) {
    if (rounds <= 0) { 'timer_done' }
    else {
        p = interval(100000, fn() { cap })
        exit_proc(p, 'killed')
        sleep(1)
        timer_cancel_loop(rounds - 1, cap)
    }
}

# ── (d) pmap message ring ────────────────────────────────────────────────

fun pmap_worker(cap, x) { {x, big(cap, 2)} }

fun pmap_ring(rounds, cap, lst) {
    if (rounds <= 0) { 'pmap_done' }
    else {
        _r = pmap(fn(x) { pmap_worker(cap, x) }, lst)
        pmap_ring(rounds - 1, cap, lst)
    }
}

# ── (e) Spawn storm ──────────────────────────────────────────────────────

fun spawn_storm(n) {
    if (n <= 0) { 'spawn_done' }
    else {
        spawn(0)
        spawn_storm(n - 1)
    }
}

# ── Coordination: report to parent, each worker is independent ───────────

fun worker(parent, id, deadline_ms, cap, tid, lst) {
    sup_storm(3, cap)
    ets_churn(tid, 500, cap)
    timer_cancel_loop(20, cap)
    pmap_ring(10, cap, lst)
    spawn_storm(200)
    now = timestamp()
    if (now < deadline_ms) {
        worker(parent, id, deadline_ms, cap, tid, lst)
    } else {
        send(parent, {'worker_done', id})
    }
}

fun collect(n) {
    if (n <= 0) { 0 }
    else {
        receive {
            {'worker_done', _id} -> collect(n - 1)
            after 300000 { print("SOAK_FAIL: worker timeout"); sys_exit(1) }
        }
    }
}

fun soak_seconds(args) {
    if (length(args) >= 2) { to_int(hd(tl(args))) }
    else { 60 }
}

fun main() {
    args = os_args()
    secs = soak_seconds(args)
    deadline = timestamp() + secs * 1000

    print("soak_mixed: starting " ++ to_string(secs) ++ "s soak")

    cap = big("c", 64)    # ~1 KB per worker
    lst = [1, 2, 3, 4, 5, 6, 7, 8]
    me = self()

    # One shared ETS table across all workers
    tid = ets_new()

    # Spawn N workers (bounded — don't exhaust the slab)
    n_workers = 4
    spawn_workers(me, n_workers, deadline, cap, tid, lst, n_workers)

    collect(n_workers)

    print("SOAK_OK secs=" ++ to_string(secs))
    sys_exit(0)
}

fun spawn_workers(parent, n, deadline, cap, tid, lst, total) {
    if (n <= 0) { 0 }
    else {
        spawn(worker(parent, total - n, deadline, cap, tid, lst))
        spawn_workers(parent, n - 1, deadline, cap, tid, lst, total)
    }
}
