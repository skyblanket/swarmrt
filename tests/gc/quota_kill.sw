# quota_kill.sw — bidirectional gate for the PER-PROCESS MEMORY QUOTA
# (SW_PROC_MEM_MAX, Phase 3 limits & quotas).
#
# A hog child accumulates ~6.5 MB of LIVE strings (list-prepend, so every
# byte is carried forward across turn checkpoints — nothing is reclaimable).
# Under a small quota (the gate runs SW_PROC_MEM_MAX=2000000) the hog's arena
# grow/adopt path trips sw_varena_quota_check and the PROCESS dies with a loud
# panic naming the quota + pid — while a sibling process and the parent (root)
# complete normally: the node survives, fault isolation holds, a supervisor
# could restart the hog. With the quota UNSET the same binary completes the
# full hog. Both runs print PROBE_OK; the Makefile gate additionally greps
# stderr for the SW_PROC_MEM_MAX banner (present when capped, absent when
# uncapped — no false kills).
#
# Bidirectional by construction: neuter sw_varena_quota_check and the capped
# run FAILS (hog_done arrives, no DOWN-with-quota-reason, no stderr banner).
module Main

fun big_string(acc, n) {
    if (n <= 0) { acc }
    else { big_string(acc ++ "0123456789abcdef", n - 1) }
}

# Memory hog: accumulate n ~64KB strings in a live list (the recursion arg),
# so the per-turn checkpoint must carry the whole set forward — arena growth
# is real retained memory, not per-turn garbage.
fun hog(parent, list, n) {
    if (n <= 0) { send(parent, {'hog_done', length(list)}) 0 }
    else { hog(parent, [big_string("c", 4096) | list], n - 1) }
}

# Sibling: small allocations only — must complete under any sane quota.
fun sibling(parent) {
    send(parent, {'sib_done', string_length(big_string("s", 4))})
    0
}

fun capped_run(hpid) {
    # The hog must DIE (monitor DOWN), the sibling must COMPLETE, and
    # hog_done must never arrive.
    down_ok = receive {
        {'DOWN', _, _, dpid, _reason} -> if (dpid == hpid) { 1 } else { 0 }
        after 30000 { 0 }
    }
    sib_ok = receive { {'sib_done', _} -> 1 after 30000 { 0 } }
    hog_leak = receive { {'hog_done', _} -> 1 after 500 { 0 } }
    if (down_ok == 1 && sib_ok == 1 && hog_leak == 0) {
        print("quota_kill capped: hog killed, sibling + parent survived")
        print("PROBE_OK")
        sys_exit(0)
    } else {
        print(f"FAIL quota_kill capped: down_ok={down_ok} sib_ok={sib_ok} hog_leak={hog_leak}")
        sys_exit(1)
    }
}

fun uncapped_run() {
    # No quota: the hog must COMPLETE (all 100 rounds) and the sibling too.
    hog_ok = receive {
        {'hog_done', cnt} -> if (cnt == 100) { 1 } else { 0 }
        after 60000 { 0 }
    }
    sib_ok = receive { {'sib_done', _} -> 1 after 30000 { 0 } }
    if (hog_ok == 1 && sib_ok == 1) {
        print("quota_kill uncapped: hog completed all rounds")
        print("PROBE_OK")
        sys_exit(0)
    } else {
        print(f"FAIL quota_kill uncapped: hog_ok={hog_ok} sib_ok={sib_ok}")
        sys_exit(1)
    }
}

fun main() {
    me = self()
    hpid = spawn(hog(me, [], 100))     # ~100 x 64KB live = ~6.5 MB retained
    _ref = monitor(hpid)
    spawn(sibling(me))
    q = getenv("SW_PROC_MEM_MAX")
    case q {
        'nil' -> uncapped_run()
        "0"   -> uncapped_run()
        _     -> capped_run(hpid)
    }
}
