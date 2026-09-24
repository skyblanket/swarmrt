module Test_blocking_section

# A builtin that blocks its OS thread (terminal input, a synchronous wait on
# a child's pipe) used to strand every process queued on that scheduler:
# there is no general work stealing, so they waited for the builtin to
# return even with every other scheduler idle. swarm-code's interactive
# session hung this way — the agent's home scheduler was the one its
# line-reader occupied. Blocking builtins now hand their queue to idle
# schedulers (sw_blocking_enter).
#
# Re-runs itself with SW_SCHEDULERS=2 so the blocker and the workers are
# guaranteed to share a scheduler: one process holds a thread for 3s in
# subprocess_recv_line; 16 short workers must still all report back fast.

fun worker(parent, i) { send(parent, {'done', i}) }

fun blocker(parent) {
    h = subprocess_spawn("sleep 10")
    send(parent, {'blocking'})
    subprocess_recv_line(h, 3000)
    subprocess_close(h)
}

fun spawn_workers(i, n, me) {
    if (i < n) {
        spawn(worker(me, i))
        spawn_workers(i + 1, n, me)
    } else { 'ok' }
}

fun collect(left) {
    if (left == 0) { 'ok' }
    else {
        receive {
            {'done', _i} -> collect(left - 1)
            after 5000 -> 'timeout'
        }
    }
}

fun child() {
    me = self()
    spawn(blocker(me))
    receive { {'blocking'} -> 'ok' after 2000 -> 'no_blocker' }
    sleep(50)
    t0 = timestamp()
    spawn_workers(0, 16, me)
    r = collect(16)
    ms = timestamp() - t0
    print(f"RESULT {r} {ms}")
    sys_exit(0)
}

fun main() {
    if (getenv("SW_BLOCKING_CHILD") == "1") { child() }
    else {
        me = hd(os_args())
        out = elem(shell("SW_BLOCKING_CHILD=1 SW_SCHEDULERS=2 '" ++ me ++ "' 2>&1"), 1)
        parts = string_split(string_trim(out), " ")
        fails = 0
        if (length(parts) == 3 && hd(parts) == "RESULT" && hd(tl(parts)) == "ok") {
            print("PASS workers_all_reported")
            ms = to_int(hd(tl(tl(parts))))
            if (ms < 1500) { print(f"PASS not_stranded_behind_blocker ({ms}ms)") }
            else { print(f"FAIL not_stranded_behind_blocker: {ms}ms (blocker holds 3000ms)") ; fails = fails + 1 }
        } else {
            print(f"FAIL workers_all_reported: {out}")
            fails = fails + 2
        }
        if (fails == 0) { print("OK blocking_section 2/2") ; sys_exit(0) }
        else { print("FAIL blocking_section") ; sys_exit(1) }
    }
}
