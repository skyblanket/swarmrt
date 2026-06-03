module Test_task_stream

# Regression for Std.task_stream — the bounded parallel fan-out primitive.
#
# Guards three properties pmap CANNOT give you:
#   1. results come back IN INPUT ORDER, every cell tagged
#      {'ok',_} / {'error',_} — NEVER a bare nil (pmap's silent-nil bug);
#   2. a slow item that overruns timeout_ms yields {'error','timeout'},
#      not nil, and does not stall the fast items;
#   3. concurrency NEVER exceeds max_concurrency — proven by an ETS gauge
#      that each worker bumps on entry / drops on exit, recording the peak.

import Std

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# 0-indexed LIST accessor — `elem` is the TUPLE accessor and panics on a
# list. task_stream returns a list, so we index into it with this.
fun lat(lst, i) {
    if (i == 0) { hd(lst) } else { lat(tl(lst), i - 1) }
}

# ---- 1. order + all-ok, every cell tagged ----
fun test_order_and_ok() {
    res = Std.task_stream([1, 2, 3, 4, 5], fun(x) { x * 10 },
                          %{max_concurrency: 2})
    expected = [{'ok', 10}, {'ok', 20}, {'ok', 30}, {'ok', 40}, {'ok', 50}]
    assert_eq("task_stream_order", res, expected)
}

# ---- 2. a crashing fn becomes {'error', reason}, never nil/hang ----
fun test_crash_is_tagged() {
    res = Std.task_stream([1, 2, 3], fun(x) {
        if (x == 2) { panic("boom on 2") } else { x }
    }, %{max_concurrency: 3})
    ok1 = assert_eq("crash_ok1", lat(res, 0), {'ok', 1})
    # Element 1 must be an {'error', _} tuple (reason text varies).
    bad = lat(res, 1)
    is_err = if (elem(bad, 0) == 'error') { 'true' } else { 'false' }
    ok2 = assert_eq("crash_tagged_error", is_err, 'true')
    ok3 = assert_eq("crash_ok3", lat(res, 2), {'ok', 3})
    ok1 + ok2 + ok3
}

# ---- 3. slow item -> {'error','timeout'}, fast items still {'ok',_} ----
fun test_timeout_is_tagged() {
    # Item 1 sleeps past the 300ms budget; the others are instant.
    res = Std.task_stream([1, 2, 3], fun(x) {
        if (x == 2) { sleep(3000) ; x } else { x }
    }, %{max_concurrency: 3, timeout_ms: 300, on_timeout: 'error'})
    ok1 = assert_eq("timeout_fast0", lat(res, 0), {'ok', 1})
    ok2 = assert_eq("timeout_slow1", lat(res, 1), {'error', 'timeout'})
    ok3 = assert_eq("timeout_fast2", lat(res, 2), {'ok', 3})
    ok1 + ok2 + ok3
}

# ---- 3b. NEVER a bare nil — every cell is a 2-tuple ----
fun test_no_bare_nil() {
    res = Std.task_stream([1, 2, 3, 4], fun(x) {
        if (x == 3) { sleep(3000) ; x } else { x }
    }, %{max_concurrency: 2, timeout_ms: 250})
    assert_eq("no_bare_nil", Std.all(res, fun(c) {
        if (c != nil && (elem(c, 0) == 'ok' || elem(c, 0) == 'error')) { 'true' } else { 'false' }
    }), 'true')
}

# ---- 4. concurrency cap is honoured (ETS peak gauge) ----
# Each worker increments a shared "live" counter, records the running max,
# sleeps briefly so overlap is forced, then decrements. With 10 items and
# max_concurrency 3, the peak must be <= 3.
let gauge = ets_new()

# Atomic gauge: ets_update_counter is the lock-free primitive. `live` is
# incremented on entry / decremented on exit; `peak` is raised to the new
# `live` via a compare-and-swap retry loop (so concurrent bumps can't lose
# the max). This is what actually proves the cap, race-free.
fun bump_and_work(x) {
    live = ets_update_counter(gauge, 'live', 1, 0)
    raise_peak(live)
    sleep(40)
    ets_update_counter(gauge, 'live', -1, 0)
    x * 2
}

fun raise_peak(live) {
    cur = case ets_get(gauge, 'peak') { nil -> 0 ; p -> p }
    if (live <= cur) { 'ok' }
    else {
        if (ets_cas(gauge, 'peak', cur, live) == 'true') { 'ok' }
        else { raise_peak(live) }   # someone else moved it — retry
    }
}

fun test_concurrency_cap() {
    ets_put(gauge, 'live', 0)
    ets_put(gauge, 'peak', 0)
    res = Std.task_stream(Std.range(0, 10), fun(x) { bump_and_work(x) },
                          %{max_concurrency: 3})
    peak = ets_get(gauge, 'peak')
    ok_count = assert_eq("cap_result_count", length(res), 10)
    ok_first = assert_eq("cap_first", lat(res, 0), {'ok', 0})
    ok_last = assert_eq("cap_last", lat(res, 9), {'ok', 18})
    # The whole point: at most max_concurrency workers ever ran at once.
    ok_peak = if (peak <= 3 && peak >= 1) { print("PASS cap_peak_le_3 (peak=" ++ to_string(peak) ++ ")") ; 0 }
              else { print("FAIL cap_peak_le_3: peak=" ++ to_string(peak)) ; 1 }
    ok_count + ok_first + ok_last + ok_peak
}

# ---- 5. empty list -> empty list ----
fun test_empty() {
    assert_eq("empty", Std.task_stream([], fun(x) { x }, %{}), [])
}

# ---- 6. json_expect: decode + required-key validation ----
fun test_json_expect_ok() {
    case Std.json_expect("{\"name\":\"a\",\"age\":3}", %{required: ['name', 'age']}) {
        {'ok', m} -> assert_eq("json_expect_ok", map_get(m, 'name'), "a")
        _         -> assert_eq("json_expect_ok", "error", "ok")
    }
}

fun test_json_expect_missing() {
    case Std.json_expect("{\"name\":\"a\"}", %{required: ['name', 'age']}) {
        {'error', _} -> assert_eq("json_expect_missing", 'true', 'true')
        _            -> assert_eq("json_expect_missing", "ok", "error")
    }
}

fun test_json_expect_bad() {
    case Std.json_expect("not json at all", %{required: []}) {
        {'error', _} -> assert_eq("json_expect_bad", 'true', 'true')
        _            -> assert_eq("json_expect_bad", "ok", "error")
    }
}

fun main() {
    fails = 0
    fails = fails + test_order_and_ok()
    fails = fails + test_crash_is_tagged()
    fails = fails + test_timeout_is_tagged()
    fails = fails + test_no_bare_nil()
    fails = fails + test_concurrency_cap()
    fails = fails + test_empty()
    fails = fails + test_json_expect_ok()
    fails = fails + test_json_expect_missing()
    fails = fails + test_json_expect_bad()
    if (fails == 0) { print("OK task_stream 15/15") ; sys_exit(0) }
    else { print("FAIL task_stream " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
