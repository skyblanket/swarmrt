# ets_list_uaf.sw — does ets_list() copy OUT like ets_get, or alias the table?
# Single process, SW_SCHEDULERS=1: put a fat value, ets_list to capture the
# {k,v} pairs, then REPLACE the key many times + delete it (each replace/delete
# frees the table's stored value graph). Finally deep-read the captured list.
# If ets_list aliased the table's stored value, this walks freed/poisoned
# memory -> ASAN use-after-free under -DSW_ARENA_POISON.
module Main

fun big(acc, n) { if (n <= 0) { acc } else { big(acc ++ "ABCDEFGHIJKLMNOP", n - 1) } }
fun fat(tag) { {tag, big("v", 24), [1, 2, 3, 4, 5], %{"k" => big("m", 12)}} }

fun churn(tid, rounds) {
    if (rounds <= 0) { ets_delete(tid, 7) }
    else { ets_put(tid, 7, fat("churn")); churn(tid, rounds - 1) }
}

fun main() {
    print("ets_list_uaf start (run with SW_SCHEDULERS=1)")
    tid = ets_new()
    ets_put(tid, 7, fat("ORIGINAL"))
    pairs = ets_list(tid)          # capture aliased-or-copied {k,v} pairs
    churn(tid, 200)                # free the table's stored value repeatedly, then delete
    s = to_string(pairs)           # deep-read the captured pairs -> UAF if aliased
    if (length(s) > 0) { print("PROBE_OK list value intact, len=" ++ to_string(length(s))); sys_exit(0) }
    else { print("PROBE_FAIL empty"); sys_exit(1) }
}
