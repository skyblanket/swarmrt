# slope_ets.sw — memory-slope probe for ETS value ownership (Ownership v2).
# A long-lived process churns a FIXED-size table: it repeatedly put()s (replaces)
# moderately-sized values at a small set of hot keys, bumps counters, deletes, and
# get()s (read). The LIVE set stays tiny (a few dozen keys), so peak RSS must be
# FLAT in the number of rounds.
#
# Before the fix: ets_put replace overwrote the stored value without freeing it,
# and ets_delete freed only the entry struct — so every replace/delete leaked a
# global-heap value graph and RSS grew linearly with rounds. After: the table
# frees its old value on replace/delete/take and copies values OUT to readers, so
# memory is bounded by the live set, not cumulative churn.
module Main

fun big_string(acc, n) { if (n <= 0) { acc } else { big_string(acc ++ "0123456789abcdef", n - 1) } }

# A ~512-byte structured value (string + list + map) so a leaked copy is visible.
fun payload(r) {
    {"row", big_string("v", 16), [r, r + 1, r + 2, r + 3], %{"k" => big_string("m", 8), "n" => r}}
}

fun churn(tid, rounds) {
    if (rounds <= 0) { 0 }
    else {
        k = rounds % 32                              # 32 hot keys -> small live set
        ets_put(tid, k, payload(rounds))             # insert or REPLACE (frees old post-fix)
        ets_update_counter(tid, 1000 + (rounds % 8), 1, 0)   # counter replace churn
        g = ets_get(tid, k)                          # read -> copy-out into our arena
        if (rounds % 4 == 0) { ets_delete(tid, (rounds + 7) % 32) }  # delete churn (frees value+key)
        if (rounds % 9 == 0) { ets_take(tid, (rounds + 3) % 32) }    # take churn (frees value+key)
        churn(tid, rounds - 1)
    }
}

fun job_count(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 4000 } }

fun main() {
    rounds = job_count(os_args())
    tid = ets_new()
    print("slope_ets rounds=" ++ to_string(rounds))
    churn(tid, rounds)
    print("slope_ets DONE count=" ++ to_string(ets_count(tid)))
    print("PROBE_OK")
    0
}
