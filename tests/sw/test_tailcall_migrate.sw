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

# Deterministic half: build and run the C regression test
# (tests/stress/tc_park_per_process.c), which interleaves two fibers on ONE
# scheduler thread through the park. It does not compile against a __thread
# park (no sw_tc_park), so this test fails on the old implementation even where
# the compiler happens not to cache the TLS address.
fun park_probe() {
    cmd = "R=.; [ -f src/swarmrt_lang.h ] || R=../..; " ++
          "cc -O2 -pthread -DSWARMRT_TLS -I$R/src $R/tests/stress/tc_park_per_process.c " ++
          "$R/bin/libswarmrt.a -lssl -lcrypto -lsqlite3 -lz -lm -o /tmp/tc_park_probe_$$ 2>&1 && " ++
          "SW_SCHEDULERS=1 /tmp/tc_park_probe_$$ 2>&1; rm -f /tmp/tc_park_probe_$$"
    out = elem(shell(cmd), 1)
    if (string_contains(out, "OK tc_park_per_process")) { 0 } else { print(out) ; 1 }
}

fun main() {
    probe_bad = park_probe()
    me = self()
    spawn_all(64, me)
    bad = collect(64, 0) + probe_bad
    if (bad == 0) { print("OK tailcall_migrate 64/64") ; sys_exit(0) }
    else { print("FAIL tailcall_migrate bad=" ++ to_string(bad)) ; sys_exit(1) }
}
