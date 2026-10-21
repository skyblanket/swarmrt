module Test_shell_status_concurrent

# shell()'s wrapper creates its exit file before writing the status, and the
# poller used to take the file's existence as "done": a poll landing in that
# gap read "" -> 0, so a failed command came back as exit 0 under concurrency.
# Also: a command that changes PATH must not break the wrapper.

fun worker(parent, n, bad) {
    if (n == 0) { send(parent, {'done', bad}) } else {
        st = elem(shell("false"), 0)
        worker(parent, n - 1, if (st == 1) { bad } else { bad + 1 })
    }
}
fun collect(k, total) { if (k == 0) { total } else { receive { {'done', b} -> collect(k - 1, total + b) } } }
fun spawn_n(me, k) { if (k == 0) { "" } else { _s = spawn(worker(me, 50, 0)); spawn_n(me, k - 1) } }

fun check(name, cond) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name) ; 1 }
}

fun main() {
    f = 0
    t0 = timestamp()
    r = shell("export PATH=/nonexistent; echo hi; false")
    f = f + check("path_changed", if (elem(r, 0) == 1 && elem(r, 1) == "hi\n" && timestamp() - t0 < 5000) { 'true' } else { 'false' })
    me = self()
    _a = spawn_n(me, 32)
    bad = collect(32, 0)
    f = f + check("concurrent_false_is_1", if (bad == 0) { 'true' } else { 'false' })
    if (f == 0) { print("OK shell_status_concurrent 2/2") ; sys_exit(0) } else { print("FAIL shell_status_concurrent") ; sys_exit(1) }
}
