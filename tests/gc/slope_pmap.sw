# slope_pmap.sw — memory-slope probe for pmap (Ownership v2). A long-lived caller
# repeatedly pmaps a closure over a list; each worker builds garbage and returns a
# value. Captures must go in worker-adopted spawn regions, results adopted into the
# caller. Before v2 fix: pmap leaked both globally (grew with rounds*items).
module Main
fun big_string(acc, n) { if (n <= 0) { acc } else { big_string(acc ++ "0123456789abcdef", n - 1) } }
fun loop(rounds, lst, acc) {
    if (rounds <= 0) { acc }
    else {
        r = pmap(fn(x) { {x, big_string("v", 64)} }, lst)
        loop(rounds - 1, lst, acc + length(r))
    }
}
fun job_count(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 2000 } }
fun main() {
    rounds = job_count(os_args())
    lst = [1, 2, 3, 4, 5, 6, 7, 8]
    print("slope_pmap rounds=" ++ to_string(rounds))
    final = loop(rounds, lst, 0)
    print("slope_pmap DONE " ++ to_string(final))
    print("PROBE_OK")
    0
}
