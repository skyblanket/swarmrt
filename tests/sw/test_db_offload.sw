module Test_db_offload

# Regression: db_open / db_exec / db_query / db_close must NOT pin their
# scheduler thread. A long query used to run on the scheduler OS thread, so
# with every scheduler inside one, nothing else ran (a 100ms timer fired
# after ~7s under SW_SCHEDULERS=2). The builtins now run on the runtime's
# offload pool while the caller parks.
#
# Two processes each run a ~1s recursive-CTE query on one shared handle. A
# third process sleeps 100ms and reports when it woke. The timer must fire
# well before either query finishes. The test must pass even with
# SW_SCHEDULERS=2 (run it that way to exercise the bug's trigger).

fun busy(parent, h, i) {
    t0 = timestamp()
    rows = db_query(h, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 8000000) SELECT count(*) AS n FROM c", [])
    send(parent, {'busy', i, timestamp() - t0, rows})
}

fun tick(parent, t0) {
    sleep(100)
    send(parent, {'tick', timestamp() - t0})
}

fun collect(n, busy_ms, tick_ms, fails) {
    if (n == 0) { report(busy_ms, tick_ms, fails) }
    else {
        receive {
            {'busy', _i, ms, rows} -> {
                ok = length(rows) == 1 && map_get(hd(rows), "n") == 8000000
                if (ok) { print("PASS query_rows") ; collect(n - 1, if (ms > busy_ms) { ms } else { busy_ms }, tick_ms, fails) }
                else { print("FAIL query_rows") ; collect(n - 1, busy_ms, tick_ms, fails + 1) }
            }
            {'tick', ms} -> collect(n - 1, busy_ms, ms, fails)
            after 20000 -> report(busy_ms, tick_ms, fails + 1)
        }
    }
}

fun report(busy_ms, tick_ms, fails) {
    f = fails
    if (tick_ms > 0 && tick_ms < 500 && tick_ms < busy_ms) { print("PASS timer_fires") }
    else { print(f"FAIL timer_fires: 100ms timer fired at {tick_ms}ms (queries took {busy_ms}ms)") ; f = f + 1 }
    if (f == 0) { print("OK db_offload 3/3") ; sys_exit(0) }
    else { print("FAIL db_offload") ; sys_exit(1) }
}

fun main() {
    h = db_open(":memory:")
    if (h < 0) { print("FAIL db_open") ; sys_exit(1) }
    t0 = timestamp()
    spawn(busy(self(), h, 1))
    spawn(busy(self(), h, 2))
    spawn(tick(self(), t0))
    collect(3, 0, 0, 0)
}
