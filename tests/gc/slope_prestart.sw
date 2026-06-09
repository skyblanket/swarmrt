# slope_prestart.sw — pre-start-kill spawn-region reclaim (Ownership v2), with
# BOUNDED concurrency: spawn a worker with a ~32KB arg, exit_proc it, then yield
# (sleep 1ms) so it is reaped before the next spawn — so peak-live workers stays ~1
# and any RSS growth is a leaked spawn region, not in-flight concurrency.
module Main
fun big_string(acc, n) { if (n <= 0) { acc } else { big_string(acc ++ "0123456789abcdef0123456789abcdef", n - 1) } }
fun worker(arg) { string_length(arg) }
fun loop(rounds, big) {
    if (rounds <= 0) { 0 }
    else {
        p = spawn(worker(big))
        exit_proc(p, 'kill')
        sleep(1)
        loop(rounds - 1, big)
    }
}
fun job_count(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 3000 } }
fun main() {
    rounds = job_count(os_args())
    big = big_string("", 1024)
    print("slope_prestart rounds=" ++ to_string(rounds))
    loop(rounds, big)
    print("slope_prestart DONE")
    print("PROBE_OK")
    0
}
