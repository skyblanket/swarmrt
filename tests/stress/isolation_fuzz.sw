# isolation_fuzz.sw — cross-process isolation under malformed input.
#
# Phase 3 exit criterion: "malformed input cannot crash another process."
# The parser/JSON/marshal fuzz targets prove the DECODERS are memory-safe in
# isolation; this proves the RUNTIME PROPERTY around them — a process that
# ingests hostile input and dies (panic on a decode-shaped bad access, or a
# clean error return) does NOT corrupt, stall, or kill its siblings or the
# node. That is the actual guarantee an operator relies on: one poisoned
# request kills one request, nothing else.
#
# Layout per round: spawn a batch of CRASHER workers each fed a different
# adversarial input (deeply-nested JSON → depth cap, raw garbage bytes,
# truncated/oversized shapes, then a hard hd([])/elem-OOB panic to force an
# abnormal exit), all monitored; alongside them a SURVIVOR worker doing real
# work (build a list, decode well-formed JSON, sum) that must return the
# right answer, and a long-lived ETS table the survivor reads/writes across
# the crashes. After each round assert: every crasher delivered a DOWN, the
# survivor's answer is exact, and the ETS value is intact. Run many rounds so
# a corruption that only shows under churn has a chance to surface.
module Main

# A crasher: touch the malformed input a few ways, then force an abnormal
# exit. json_decode of garbage / deep nesting returns nil (safe) — the panic
# is what exercises the ISOLATION path (abnormal exit must stay local).
fun crasher(payload) {
    _a = json_decode(payload)
    _b = json_decode(payload ++ "]]]]]]]]]]")
    _c = string_length(payload)
    # Force an abnormal exit — hd of the empty list panics uncatchably.
    _d = hd([])
    0
}

# Survivor: real work that must be UNAFFECTED by the crashers. Decode a
# well-formed object, walk a list, hit the shared ETS table.
fun sum_list(list, acc) {
    case list {
        [] -> acc
        [h | t] -> sum_list(t, acc + h)
    }
}

fun survivor(parent, tid, round) {
    decoded = json_decode("{\"n\": 42, \"ok\": true}")
    n = map_get(decoded, "n")
    total = sum_list([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 0)   # = 55
    ets_put(tid, "round", round)
    back = ets_get(tid, "round")
    ok = if (n == 42 && total == 55 && back == round) { 1 } else { 0 }
    send(parent, {'survived', ok})
    0
}

# Adversarial inputs — the shapes the decoders must survive AND stay isolated
# around. Kept as a small rotating set indexed by worker number.
fun payload_for(i) {
    case (i % 6) {
        0 -> "[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]"
        1 -> "\xff\xfe\x00garbage\x01\x02not json at all"
        2 -> "{\"unterminated\": "
        3 -> "{\"deep\": {\"deep\": {\"deep\": {\"deep\": {\"x\": 1}}}}}"
        4 -> "123456789012345678901234567890.99999e999999"
        _ -> "\"a string with \\ bad \\x escapes and no close"
    }
}

# Collect N DOWNs (one per crasher). Returns the count seen.
fun await_downs(n, seen) {
    if (n <= 0) { seen }
    else {
        got = receive {
            {'DOWN', _, _, _, _} -> 1
            after 15000 { 0 }
        }
        if (got == 1) { await_downs(n - 1, seen + 1) }
        else { seen }
    }
}

fun spawn_crashers(n) {
    if (n <= 0) { 0 }
    else {
        p = spawn(crasher(payload_for(n)))
        _r = monitor(p)
        spawn_crashers(n - 1)
    }
}

fun run_round(tid, round) {
    me = self()
    ncrash = 8
    spawn_crashers(ncrash)
    spawn(survivor(me, tid, round))

    downs = await_downs(ncrash, 0)
    surv = receive { {'survived', ok} -> ok after 15000 { 0 } }
    ets_val = ets_get(tid, "round")

    if (downs == ncrash && surv == 1 && ets_val == round) {
        1
    } else {
        print(f"FAIL round {round}: downs={downs}/{ncrash} surv={surv} ets={ets_val}")
        0
    }
}

fun loop_rounds(tid, round, total, ok_so_far) {
    if (round > total) { ok_so_far }
    else {
        r = run_round(tid, round)
        loop_rounds(tid, round + 1, total, ok_so_far + r)
    }
}

fun main() {
    tid = ets_new("iso")
    rounds = 25
    ok = loop_rounds(tid, 1, rounds, 0)
    if (ok == rounds) {
        print(f"isolation_fuzz: all {rounds} rounds clean — crashers isolated, survivor + ETS intact")
        print("PROBE_OK")
        sys_exit(0)
    } else {
        print(f"FAIL isolation_fuzz: {ok}/{rounds} rounds clean")
        sys_exit(1)
    }
}
