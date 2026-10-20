module Test_db_offload

# db_query used to hold its scheduler OS thread for the whole call, so with N
# schedulers only N slow queries could run at once. It now runs on the offload
# pool while the caller parks.
#
# The slow part here is SQLite waiting on a lock: the child holds an EXCLUSIVE
# transaction on a file database, and each reader gives up after 300ms
# (busy_timeout). The wait is off-CPU, so the overlap does not depend on the
# core count. A CPU-bound query would not show it on a throttled host, where
# eight concurrent sqlite3 runs took about seven times one run.
#
# Re-runs itself with SW_SCHEDULERS=2. Eight readers, each waiting 500ms:
# two at a time (the scheduler cap) they take ~2.0s; offloaded they overlap
# in about three waves or fewer (~1.0-1.5s).

fun reader(parent, i, path) {
    h = db_open(path)
    db_exec(h, "PRAGMA busy_timeout = 500")
    rows = db_query(h, "SELECT count(*) AS n FROM t")
    db_close(h)
    send(parent, {'done', i, length(rows)})
}

fun spawn_readers(i, n, me, path) {
    if (i < n) {
        spawn(reader(me, i, path))
        spawn_readers(i + 1, n, me, path)
    } else { 'ok' }
}

fun collect(left, ok) {
    if (left == 0) { ok }
    else {
        receive {
            {'done', _i, _rows} -> collect(left - 1, ok + 1)
            after 10000 -> ok
        }
    }
}

fun child() {
    me = self()
    path = file_temp("/tmp/sw_db_offload_")
    hold = db_open(path)
    db_exec(hold, "CREATE TABLE IF NOT EXISTS t (x INTEGER)")
    db_exec(hold, "BEGIN EXCLUSIVE")
    t0 = timestamp()
    spawn_readers(0, 8, me, path)
    ok = collect(8, 0)
    ms = timestamp() - t0
    db_exec(hold, "ROLLBACK")
    db_close(hold)
    file_delete(path)
    print(f"RESULT {ok} {ms}")
    sys_exit(0)
}

fun main() {
    if (getenv("SW_DB_OFFLOAD_CHILD") == "1") { child() }
    else {
        me = hd(os_args())
        out = elem(shell("SW_DB_OFFLOAD_CHILD=1 SW_SCHEDULERS=2 '" ++ me ++ "' 2>&1"), 1)
        parts = string_split(string_trim(out), " ")
        fails = 0
        if (length(parts) == 3 && hd(parts) == "RESULT") {
            ok = to_int(hd(tl(parts)))
            ms = to_int(hd(tl(tl(parts))))
            if (ok == 8) { print("PASS all_8_readers_reported") }
            else { print(f"FAIL all_8_readers_reported: {ok}/8") ; fails = fails + 1 }
            if (ms < 1750) { print(f"PASS db_waits_overlap_on_2_schedulers ({ms}ms)") }
            else { print(f"FAIL db_waits_overlap_on_2_schedulers: {ms}ms (two at a time: ~2000ms)") ; fails = fails + 1 }
        } else {
            print(f"FAIL all_8_readers_reported: {out}")
            fails = fails + 2
        }
        if (fails == 0) { print("OK db_offload 2/2") ; sys_exit(0) }
        else { print("FAIL db_offload") ; sys_exit(1) }
    }
}
