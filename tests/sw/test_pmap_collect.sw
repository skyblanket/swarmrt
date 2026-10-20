module Test_pmap_collect

# Regression for the pmap collector. It used to take ANY message from the
# caller's mailbox, so:
#   1. a stray {int, x} message was consumed as a result;
#   2. a worker that missed the window delivered into a LATER pmap call;
#   3. the 5s wait restarted on every receive, so the wall time was n x 5s
#      and a slow item silently became nil.
# Results are now matched on a per-call ref, and the whole call shares one
# 5s deadline. A slow item is {'error', 'timeout'}, as in Std.task_stream.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# A message shaped like a pmap result, sent to the caller before the call.
fun test_stray_message() {
    send(self(), {0, 'x'})
    res = pmap(fun(x) { x * 2 }, [1, 2, 3])
    assert_eq("pmap_stray_ignored", res, [2, 4, 6])
}

# pmap #1 gives up on a slow item; its worker delivers ~200ms later, while
# pmap #2 is still waiting. pmap #2 must not take that result as its own.
fun test_late_worker_no_leak() {
    r1 = pmap(fun(x) {
        if (x == 0) { sleep(5200) }
        x * 10
    }, [0, 1])
    f1 = assert_eq("pmap_slow_is_error", r1, [{'error', 'timeout'}, 10])
    r2 = pmap(fun(x) {
        if (x == 1) { sleep(1000) }
        x + 100
    }, [0, 1])
    f1 + assert_eq("pmap_no_leak_into_next", r2, [100, 101])
}

# Four items finish at 1.5s, 3s, 4.5s and 6s. The old per-receive wait
# would take 6s; one shared 5s deadline caps the call and times out the last.
fun test_total_time_bounded() {
    t0 = timestamp()
    res = pmap(fun(x) {
        sleep(x * 1500)
        x
    }, [1, 2, 3, 4])
    ms = timestamp() - t0
    f1 = assert_eq("pmap_total_bounded", ms < 5500, 'true')
    f1 + assert_eq("pmap_deadline_result", res, [1, 2, 3, {'error', 'timeout'}])
}

# Order matters on unfixed code: each test leaves its leftover messages in the
# mailbox for the next, so the clean-mailbox tests run first.
fun main() {
    fails = 0
    fails = fails + test_total_time_bounded()
    fails = fails + test_late_worker_no_leak()
    fails = fails + test_stray_message()
    if (fails == 0) { print("OK pmap_collect 5/5") ; sys_exit(0) }
    else { print("FAIL pmap_collect " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
