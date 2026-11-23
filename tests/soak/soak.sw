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
#
# Self-reporting (#48): every SOAK_REPORT_SECONDS (default 60; 0 disables)
# it prints one heartbeat line between rounds,
#   SOAK_STAT {"t_ms":..,"rounds":..,"processes":..,"crashes":..,...}
# from swarm_stats(), which tests/soak/signoff.py turns into a time series
# and a verdict. SOAK_LEAK_KB=N (test-only) deliberately keeps N KB per
# round alive in main, so the sign-off's leak detector can be shown to fire.
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
    ets_drop(t)              # tables are finite (1024 live); a round must not leak one

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

# cfg = {deadline_ms, report_ms, start_ms, leak_chunk}; leak_chunk is "" unless
# SOAK_LEAK_KB is set. `kept` only grows when leaking.
fun loop_until(cfg, payload, rounds, next_report, kept) {
    {deadline_ms, report_ms, start_ms, leak_chunk} = cfg
    now = timestamp()
    if (now >= deadline_ms) {
        report(start_ms, rounds, length(kept))
        print(f"soak DONE rounds={rounds}")
        print("PROBE_OK")
        0
    } else {
        round(payload)
        kept2 = if (leak_chunk == "") { kept } else { [leak_chunk ++ to_string(rounds) | kept] }
        if (report_ms > 0 && now >= next_report) {
            report(start_ms, rounds + 1, length(kept2))
            loop_until(cfg, payload, rounds + 1, now + report_ms, kept2)
        } else {
            loop_until(cfg, payload, rounds + 1, next_report, kept2)
        }
    }
}

fun report(start_ms, rounds, kept) {
    st = swarm_stats()
    print("SOAK_STAT " ++ json_encode(%{
        t_ms: timestamp() - start_ms, rounds: rounds, kept: kept,
        processes: map_get(st, 'processes'), spawns: map_get(st, 'spawns'),
        crashes: map_get(st, 'crashes'), restarts: map_get(st, 'restarts'),
        mailbox_dropped: map_get(st, 'mailbox_dropped'),
        msgsize_dropped: map_get(st, 'msgsize_dropped'),
        overflow_queue: map_get(st, 'overflow_queue'),
        io_ports: map_get(st, 'io_ports'),
        resident_bytes: map_get(st, 'resident_bytes')}))
}

fun secs(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 60 } }

fun env_int(name, dflt) {
    case getenv(name) { nil -> dflt  v -> to_int(v) }
}

fun main() {
    s = secs(os_args())
    payload = big("", 512)   # ~8 KB payload per message
    leak_kb = env_int("SOAK_LEAK_KB", 0)
    leak_chunk = if (leak_kb > 0) { big("", leak_kb * 64) } else { "" }
    report_ms = env_int("SOAK_REPORT_SECONDS", 60) * 1000
    print(f"soak start seconds={s}")
    start = timestamp()
    report(start, 0, 0)
    loop_until({start + s * 1000, report_ms, start, leak_chunk}, payload, 0, start + report_ms, [])
    0
}
