# lsan_lifecycle.sw — LeakSanitizer lifecycle gate (Phase 2.1).
#
# The slope gates catch UNBOUNDED growth but are blind to bounded leaks
# under their RSS noise floor, and macOS ASAN has no LSan at all — so
# every ASAN run to date used detect_leaks=0 (leak-blind, see
# PRODUCTION_ROADMAP.md). This probe is the precise complement: churn
# every lifecycle owner a modest number of rounds, exit cleanly, and let
# Linux LSan assert ZERO definitely-lost blocks at process exit.
#
# Why exit-time is the right checkpoint: parked fibers' stacks are
# invisible to LSan's reachability scan (false "leaks" while alive), but
# at clean main()-return every sw process has exited and their arenas are
# freed — anything still unreachable on the malloc heap is a REAL leak.
# Globals-reachable singletons (atom table, registry, g_swarm) are
# "still reachable", which LSan does not report by default.
#
# Per-round owners exercised:
#   - one-shot timer (delay) that FIRES        (closure freed after fire)
#   - interval timer cancelled by exit_proc    (closure freed by on_destroy,
#     incl. the pre-trampoline kill window)
#   - dynamic supervisor + child, killed       (masters/list/st via on_destroy)
#   - static supervisor + child, killed        (same)
#   - ETS put/replace/delete                   (free-on-replace/delete)
#   - plain spawn that exits normally          (arena + slab reuse)
#   - cross-process compound message           (adopted + reclaimed)
#
# ~64KB captures so anything missed is unmistakably attributable.
module Main

fun big_string(acc, n) { if (n <= 0) { acc } else { big_string(acc ++ "0123456789abcdef", n - 1) } }

fun parked(cap) { receive { _x -> 0 after 100000 { 0 } } }

fun echo_once(parent, cap) {
    receive {
        {'ping', payload} -> send(parent, {'pong', payload}) ; 0
        after 5000 { 0 }
    }
}

fun one_round(cap) {
    # one-shot delay: fires, sends, closure must be freed after firing
    delay(0, fn() { cap })
    sleep(1)

    # interval cancelled immediately (pre-trampoline kill window included)
    p = interval(100000, fn() { cap })
    exit_proc(p, 'killed')
    sleep(1)

    # dynamic supervisor + child, killed
    d = dyn_supervisor(100, 1)
    sup_start_child(d, {'w', fn() { parked(cap) }, 'permanent'})
    exit_proc(d, 'killed')

    # static supervisor + child, killed
    s = supervise('one_for_one', [{'w', fn() { parked(cap) }, 'permanent'}])
    sleep(1)
    exit_proc(s, 'killed')
    sleep(2)

    # ETS churn: put / replace / delete must free the displaced values
    t = ets_new()
    ets_put(t, 'k', cap)
    ets_put(t, 'k', cap)          # replace frees the old copy
    ets_delete(t, 'k')            # delete frees the last copy

    # plain spawn, normal exit, compound message both directions
    e = spawn(echo_once(self(), cap))
    send(e, {'ping', {cap, [1, 2, 3], %{tag: 'compound'}}})
    receive { {'pong', _} -> 0 after 5000 { 0 } }
    0
}

fun loop(rounds, cap) {
    if (rounds <= 0) { 0 }
    else { one_round(cap) ; loop(rounds - 1, cap) }
}

fun job_count(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 40 } }

fun main() {
    rounds = job_count(os_args())
    cap = big_string("c", 4096)
    print("lsan_lifecycle rounds=" ++ to_string(rounds))
    loop(rounds, cap)
    print("PROBE_OK")
    0
}
