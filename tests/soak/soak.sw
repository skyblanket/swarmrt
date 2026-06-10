# soak.sw — mixed-workload soak (Phase 2.6).
#
# Runs a representative production-shaped mix in a tight loop until a
# wall-clock budget (SW_SOAK_SECONDS, default 60) elapses, then exits 0.
# Each round exercises every long-lived ownership + teardown path at once:
#   - actor fan-out: spawn workers, send 32KB-ish payloads, collect replies
#   - supervisor: start a child, crash it (abnormal), let it restart, kill
#   - ETS churn: put/replace/delete (free-on-replace path)
#   - timers: a one-shot delay that fires, an interval started+cancelled
#   - long tail loop: the per-turn checkpoint
#
# The gate (tests/soak/run_soak.sh) samples peak RSS and asserts it stays
# bounded (the slope gates prove per-owner flatness; this proves the whole
# mix stays flat under sustained churn) and that the process exits cleanly
# (no deadlock-watchdog warning, no crash). The 60s default is the CI
# smoke; the full 24h run uses the same binary with SW_SOAK_SECONDS=86400
# on a dedicated host.
module Main

fun big(acc, n) { if (n <= 0) { acc } else { big(acc ++ "0123456789abcdef", n - 1) } }

fun worker(parent) {
    receive {
        {'job', p, from} -> send(from, {'done', p}) ; worker(parent)
        'stop' -> 'ok'
        after 5000 { 'ok' }
    }
}

fun flaky(cap) {
    receive {
        'crash' -> hd([])              # abnormal exit -> supervisor restarts
        'ping'  -> flaky(cap)
        after 100000 { 'ok' }
    }
}

fun fan(n, me, payload) {
    if (n <= 0) { 'ok' }
    else {
        w = spawn(worker(me))
        send(w, {'job', payload, me})
        receive { {'done', _} -> 0 after 2000 { 0 } }
        send(w, 'stop')
        fan(n - 1, me, payload)
    }
}

fun round(payload) {
    # fan-out spawn + message round-trips
    fan(8, self(), payload)

    # ETS put/replace/delete
    t = ets_new()
    ets_put(t, 'k', payload)
    ets_put(t, 'k', payload)
    ets_delete(t, 'k')

    # supervisor: crash a child, let it restart, then kill the supervisor
    s = supervise('one_for_one', [{'fl', fun() { flaky(payload) }, 'permanent'}])
    sleep(2)
    p = whereis('fl')
    if (p != nil) { send(p, 'crash') ; sleep(3) }
    exit_proc(s, 'killed')

    # timers
    delay(0, fun() { 'tick' })
    iv = interval(100000, fun() { 'never' })
    exit_proc(iv, 'killed')
    sleep(2)
    0
}

fun loop_until(deadline_ms, payload, rounds) {
    if (timestamp() >= deadline_ms) {
        print(f"soak DONE rounds={rounds}")
        print("PROBE_OK")
        0
    } else {
        round(payload)
        loop_until(deadline_ms, payload, rounds + 1)
    }
}

fun secs(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 60 } }

fun main() {
    s = secs(os_args())
    payload = big("", 512)   # ~8 KB payload per message
    print(f"soak start seconds={s}")
    loop_until(timestamp() + s * 1000, payload, 0)
    0
}
