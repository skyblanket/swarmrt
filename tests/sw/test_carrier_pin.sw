module Test_carrier_pin

# Phase 2a (docs/design/PROCESS_WEIGHT.md): processes run on scheduler-owned
# carrier stacks, and a process that blocks stays pinned to its carrier.
# Churn thousands of processes through every way a run can end: return,
# block in receive, sleep, panic, and kill while pinned. A carrier handed
# back in the wrong state shows up as a crash or a wrong answer here.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun quick(parent, i) { send(parent, {'done', i}) }

fun waiter(parent) {
    receive { {'go', i} -> send(parent, {'done', i}) }
}

fun sleeper(parent, i) {
    sleep(1)
    send(parent, {'done', i})
}

fun crasher() { panic("carrier test") }

fun stuck() { receive { 'never' -> 0 } }

fun collect(n, acc) {
    if (n <= 0) { acc }
    else { receive { {'done', i} -> collect(n - 1, acc + i) after 10000 { -1 } } }
}

fun spawn_quick(me, i, n) {
    if (i < n) { spawn(quick(me, i))  spawn_quick(me, i + 1, n) } else { 0 }
}

fun spawn_waiters(me, i, n, acc) {
    if (i < n) { spawn_waiters(me, i + 1, n, [spawn(waiter(me)) | acc]) } else { acc }
}

fun go_all(ps, i) {
    case ps {
        [p | rest] -> { send(p, {'go', i})  go_all(rest, i + 1) }
        [] -> 0
    }
}

fun spawn_sleepers(me, i, n) {
    if (i < n) { spawn(sleeper(me, i))  spawn_sleepers(me, i + 1, n) } else { 0 }
}

fun spawn_crashers(i, n) {
    if (i < n) { spawn(crasher())  spawn_crashers(i + 1, n) } else { 0 }
}

fun kill_stuck(i, n) {
    if (i < n) {
        p = spawn(stuck())
        sleep(0)
        exit_proc(p, 'killed')
        kill_stuck(i + 1, n)
    } else { 0 }
}

fun sum_to(n) { n * (n - 1) / 2 }

fun main() {
    me = self()
    n = 5000
    f = 0
    spawn_quick(me, 0, n)
    f = f + assert_eq("returning runs free their carrier", collect(n, 0), sum_to(n))
    ws = spawn_waiters(me, 0, n, [])
    sleep(50)
    go_all(ws, 0)
    f = f + assert_eq("pinned receivers resume", collect(n, 0), sum_to(n))
    spawn_sleepers(me, 0, 1000)
    f = f + assert_eq("pinned sleepers resume", collect(1000, 0), sum_to(1000))
    spawn_crashers(0, 1000)
    kill_stuck(0, 1000)
    sleep(50)
    spawn_quick(me, 0, n)
    f = f + assert_eq("carriers reusable after panic and kill", collect(n, 0), sum_to(n))
    if (f == 0) { print("all carrier tests passed") } else { panic("carrier tests failed") }
}
