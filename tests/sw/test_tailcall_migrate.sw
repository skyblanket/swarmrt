module Test_tailcall_migrate

# Mutual tail calls park the next call per process (not per scheduler thread):
# a body that yields (sleep / timed receive / reductions) can resume on another
# scheduler and must still drive ITS OWN parked call. 64 processes each run a
# 100k-hop ping/pong, yielding every 100th hop, and return a checksum.
# Run with SW_SCHEDULERS=4 for the full migration stress.

fun ping(n, acc, me) {
    if (n == 0) { send(me, acc) }
    else if (n % 100 == 0) {
        sleep(0)
        pong(n - 1, acc + n, me)
    } else if (n % 150 == 0) {
        receive { _ -> 0 after 0 -> 0 }
        pong(n - 1, acc + n, me)
    } else { pong(n - 1, acc + n, me) }
}

fun pong(n, acc, me) {
    if (n == 0) { send(me, acc) }
    else { ping(n - 1, acc + 2 * n, me) }
}

fun collect(k, bad) {
    if (k == 0) { bad }
    else {
        receive {
            v -> collect(k - 1, if (v == expected()) { bad } else { bad + 1 })
            after 60000 -> bad + k
        }
    }
}

fun hsum(n, acc) { if (n == 0) { acc } else { hsum(n - 1, acc + (if (n % 2 == 0) { n } else { 2 * n })) } }
fun expected() { hsum(100000, 0) }

fun spawn_all(k, me) {
    if (k == 0) { 0 }
    else { spawn(fun() { ping(100000, 0, me) }) ; spawn_all(k - 1, me) }
}

fun main() {
    me = self()
    spawn_all(64, me)
    bad = collect(64, 0)
    if (bad == 0) { print("OK tailcall_migrate 64/64") ; sys_exit(0) }
    else { print("FAIL tailcall_migrate bad=" ++ to_string(bad)) ; sys_exit(1) }
}
