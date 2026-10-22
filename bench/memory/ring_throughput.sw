module Main
# Steady-state ring throughput (bench/memory/BASELINE.md, Phase 2f). N workers
# each pass a token around the ring. Every worker counts its own hops; main
# asks all of them for their counts once the tokens are out and again HOLD_MS
# later, and prints the difference: hops/HOLD_MS, setup excluded.
#   N=99000 HOLD_MS=5000 ./ring_throughput
fun worker(i, tab, n) {
    ets_put(tab, i, self())
    receive { {'go'} -> wloop(ets_get(tab, (i + 1) % n), 0) }
}
fun wloop(next, hops) {
    receive {
        {'tok', k} -> send(next, {'tok', k + 1})  wloop(next, hops + 1)
        {'report', from} -> send(from, {'hops', hops})  wloop(next, hops)
        _o -> wloop(next, hops)
    }
}
fun mk(tab, i, n) { if (i >= n) { 'ok' } else { spawn(worker(i, tab, n))  mk(tab, i + 1, n) } }
fun each(tab, i, n, m) { if (i >= n) { 'ok' } else { send(ets_get(tab, i), m)  each(tab, i + 1, n, m) } }
fun gather(n, acc) {
    if (n == 0) { acc }
    else { receive { {'hops', h} -> gather(n - 1, acc + h) after 30000 { acc } } }
}
fun total(tab, n) { each(tab, 0, n, {'report', self()})  gather(n, 0) }
fun main() {
    n = to_int(getenv("N"))
    tab = ets_new()
    mk(tab, 0, n)
    sleep(500)
    each(tab, 0, n, {'go'})
    each(tab, 0, n, {'tok', 1})
    t0 = total(tab, n)
    sleep(to_int(getenv("HOLD_MS")))
    t1 = total(tab, n)
    print("hops=" ++ to_string(t1 - t0))
    sys_exit(0)
}
