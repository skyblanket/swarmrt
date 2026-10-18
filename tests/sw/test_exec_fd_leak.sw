module Test_exec_fd_leak

# A pipe made for one subprocess used to leak into every child forked while
# it was open (no close-on-exec). The reader then saw EOF only when the
# LAST of those children exited: a quick exec_argv / http_post / llm call
# running next to a slow one took as long as the slow one (a finished
# request looked lost). Quick calls must stay quick while slow ones fork.

fun slow(parent) { exec_argv("sleep", ["1"]) ; send(parent, 'slow_done') }

fun spawn_slow(k, parent) {
    if (k == 0) { 'ok' } else { spawn(slow(parent)) ; sleep(5) ; spawn_slow(k - 1, parent) }
}

fun quick_loop(parent) { send(parent, {'worst', quick(0, 200, 0)}) }

fun quick(i, n, worst) {
    if (i == n) { worst }
    else {
        t0 = timestamp()
        exec_argv("true", [])
        dt = timestamp() - t0
        quick(i + 1, n, if (dt > worst) { dt } else { worst })
    }
}

fun wait_slow(k) {
    if (k == 0) { 'ok' } else { receive { 'slow_done' -> wait_slow(k - 1) } }
}

fun main() {
    me = self()
    spawn(quick_loop(me))
    spawn_slow(60, me)              # 60 slow children forked over ~300 ms
    worst = receive { {'worst', w} -> w }
    wait_slow(60)
    # Each quick call takes a few ms; one that waited on a sleeping
    # sibling took about a second.
    if (worst < 600) { print("PASS quick_calls_stay_quick") ; print("OK exec_fd_leak 1/1") ; sys_exit(0) }
    else { print(f"FAIL quick_calls_stay_quick: worst {worst}ms") ; sys_exit(1) }
}
