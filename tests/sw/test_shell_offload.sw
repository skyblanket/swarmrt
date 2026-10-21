module Test_shell_offload

import Std

# shell_managed used to hold its scheduler OS thread for the whole call. It now
# runs on the offload pool while the caller parks, so 64 concurrent
# `sleep 1` calls on 2 schedulers finish in about one second instead of
# 64 / 2 = ~32 seconds.
#
# Re-runs itself with SW_SCHEDULERS=2 so the scheduler count is fixed.

fun runner(parent, i) {
    r = shell_managed("sleep 1", 5000)
    send(parent, {'done', i, elem(r, 0)})
}

fun spawn_runners(i, n, me) {
    if (i < n) {
        spawn(runner(me, i))
        spawn_runners(i + 1, n, me)
    } else { 'ok' }
}

fun collect(left, ok) {
    if (left == 0) { ok }
    else {
        receive {
            {'done', _i, 0} -> collect(left - 1, ok + 1)
            {'done', _i, _code} -> collect(left - 1, ok)
            after 60000 -> ok
        }
    }
}

fun child() {
    me = self()
    t0 = timestamp()
    spawn_runners(0, 64, me)
    ok = collect(64, 0)
    ms = timestamp() - t0
    print(f"RESULT {ok} {ms} {killed_caller()}")
    sys_exit(0)
}

# A killed caller takes its command with it: a task_stream deadline kills the
# worker while it waits in shell_managed, and the process group must not
# outlive it. Runs inside the 2-scheduler child.
fun killed_caller() {
    rs = Std.task_stream(["x"], fn(t) { shell_managed("sleep 7.25", 0) },
                         %{max_concurrency: 1, timeout_ms: 300})
    sleep(700)
    left = shell("pgrep -f 'sleep [7][.]25' >/dev/null && echo running || echo gone")
    shell("pkill -f 'sleep [7][.]25'")
    if (hd(rs) == {'error', 'timeout'} && string_trim(elem(left, 1)) == "gone") { 1 } else { 0 }
}

fun main() {
    if (getenv("SW_SHELL_OFFLOAD_CHILD") == "1") { child() }
    else {
        me = hd(os_args())
        out = elem(shell("SW_SHELL_OFFLOAD_CHILD=1 SW_SCHEDULERS=2 '" ++ me ++ "' 2>&1"), 1)
        parts = string_split(string_trim(out), " ")
        fails = 0
        if (length(parts) == 4 && hd(parts) == "RESULT") {
            ok = to_int(hd(tl(parts)))
            ms = to_int(hd(tl(tl(parts))))
            if (hd(tl(tl(tl(parts)))) == "1") { print("PASS killed_caller_kills_command") }
            else { print("FAIL killed_caller_kills_command") ; fails = fails + 1 }
            if (ok == 64) { print("PASS all_64_exit_0") }
            else { print(f"FAIL all_64_exit_0: {ok}/64") ; fails = fails + 1 }
            if (ms < 5000) { print(f"PASS shell_managed_overlaps_on_2_schedulers ({ms}ms)") }
            else { print(f"FAIL shell_managed_overlaps_on_2_schedulers: {ms}ms (two at a time: ~32000ms)") ; fails = fails + 1 }
        } else {
            print(f"FAIL all_64_exit_0: {out}")
            fails = fails + 3
        }
        if (fails == 0) { print("OK shell_offload 3/3") ; sys_exit(0) }
        else { print("FAIL shell_offload") ; sys_exit(1) }
    }
}
