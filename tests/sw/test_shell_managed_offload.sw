module Test_shell_managed_offload

# shell_managed and shell wait for their child on a worker thread, not on the
# calling scheduler. Before, N concurrent calls pinned all N schedulers and
# every other process stopped until one returned.

import Std

fun check(name, cond) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name) ; 1 }
}

fun yes(b) { if (b) { 'true' } else { 'false' } }

fun slow(i) { shell_managed("sleep 10", 20000) }

fun slow_shell(i) { shell("sleep 10") }

fun main() {
    f = 0

    r = shell_managed("echo hi", 5000)
    f = f + check("output", yes(elem(r, 0) == 0 && elem(r, 1) == "hi\n" && elem(r, 2) == 'false'))

    # four long shell_managed calls (one per CPU on a 4-CPU box) must not stop
    # a sleep(100) in another process
    _a = spawn(slow(1)); _b = spawn(slow(2)); _c = spawn(slow(3)); _d = spawn(slow(4))
    sleep(200)
    t0 = timestamp()
    sleep(100)
    dt = timestamp() - t0
    f = f + check("shell_managed_does_not_pin_schedulers", yes(dt < 2000))

    _e = spawn(slow_shell(1)); _g = spawn(slow_shell(2)); _h = spawn(slow_shell(3)); _i = spawn(slow_shell(4))
    sleep(200)
    t1 = timestamp()
    sleep(100)
    dt2 = timestamp() - t1
    f = f + check("shell_does_not_pin_schedulers", yes(dt2 < 2000))

    # a killed caller takes its child with it: a task_stream deadline kills the
    # worker while it waits in shell_managed, and the child must not outlive
    # it. (Compiled only, as this suite runs: `swc run` is single-scheduler.)
    pidf = "/tmp/sw_shell_managed_offload.pid"
    rs = Std.task_stream(["x"], fn(t) { shell_managed("echo $$ > " ++ pidf ++ "; exec sleep 7.31", 0) },
                         %{max_concurrency: 1, timeout_ms: 300})
    sleep(1000)
    pid = string_trim(elem(shell("cat " ++ pidf), 1))
    left = shell("kill -0 " ++ pid ++ " 2>/dev/null && echo running || echo gone")
    f = f + check("killed_caller_kills_child", yes(hd(rs) == {'error', 'timeout'} && string_trim(elem(left, 1)) == "gone"))
    shell("rm -f " ++ pidf)

    if (f == 0) { print("OK shell_managed_offload 4/4") ; sys_exit(0) } else { print("FAIL shell_managed_offload") ; sys_exit(1) }
}
