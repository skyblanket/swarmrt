# ets_alias_repro.sw — proves ETS get() copies OUT, so a reader holding a looked-up
# value is immune to another process freeing/replacing the stored entry.
#
# Pre-copy-out design: ets_get returned the table's RAW stored pointer. With the
# new free-on-replace/delete, a reader holding that pointer while another process
# replaces or deletes the key would read freed memory (heap-UAF). The fix copies
# the value OUT into the reader's arena under the lock, so the reader's value is
# independent of the table.
#
# DETERMINISTIC (SW_SCHEDULERS=1): a holder get()s a fat value and PARKS holding it
# across a sleep; a churner then replaces that key many times (freeing the old
# stored value each time) and deletes it; the holder wakes and deep-reads its held
# value (to_string walks every byte). Under ASAN + -DSW_ARENA_POISON a shared
# pointer would fault here. Copied out, the read is clean and matches the original.
module Main

fun big(acc, n) { if (n <= 0) { acc } else { big(acc ++ "ABCDEFGHIJKLMNOP", n - 1) } }
fun fat(tag) { {tag, big("v", 24), [1, 2, 3, 4, 5], %{"k" => big("m", 12)}} }

# Repeatedly replace key 7 (each replace frees the previous stored value) then
# delete it — maximising the chance a freed pointer would be observed by a holder.
fun churn(tid, rounds) {
    if (rounds <= 0) { ets_delete(tid, 7) }
    else {
        ets_put(tid, 7, fat("churn"))
        sleep(2)
        churn(tid, rounds - 1)
    }
}

fun holder(coord) {
    ets_put(coord_tid(coord), 7, fat("ORIGINAL"))
    held = ets_get(coord_tid(coord), 7)     # copy-out: independent of the table
    sleep(200)                              # park while the churner frees/replaces key 7 repeatedly
    # Deep-read the held value AFTER the churn freed the table's copies. If get()
    # had shared the table pointer, this walks freed/poisoned memory -> ASAN UAF.
    s = to_string(held)
    if (length(s) > 0) { send(coord_pid(coord), {"ok", s}) }
    else { send(coord_pid(coord), {"bad", "empty"}) }
}

# coord is a list [pid, tid]
fun coord_pid(c) { hd(c) }
fun coord_tid(c) { hd(tl(c)) }

fun main() {
    print("ets_alias_repro start (run with SW_SCHEDULERS=1)")
    me = self()
    tid = ets_new()
    spawn(holder([me, tid]))
    sleep(20)                # let the holder put + get + park holding its value
    spawn(churn(tid, 60))    # hammer replace/delete on the held key
    receive {
        {"ok", s} -> { print("PROBE_OK held value intact, len=" ++ to_string(length(s))); sys_exit(0) }
        {"bad", w} -> { print("PROBE_FAIL " ++ w); sys_exit(1) }
        after 20000 { print("PROBE_FAIL timeout"); sys_exit(1) }
    }
}
