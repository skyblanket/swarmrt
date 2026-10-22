module Main
# Steady-state ring throughput: N workers pass N tokens for HOLD_MS; every
# 1000th hop bumps a shared counter.
fun worker(i, tab, cnt, n) {
    ets_put(tab, i, self())
    receive { {'go'} -> wloop(ets_get(tab, (i + 1) % n), cnt) }
}
fun wloop(next, cnt) {
    receive {
        {'tok', k} -> bump(k, cnt)  send(next, {'tok', k + 1})  wloop(next, cnt)
        _o -> wloop(next, cnt)
    }
}
fun bump(k, cnt) { if (k % 1000 == 0) { ets_update_counter(cnt, 'hops', 1, 0) } else { 0 } }
fun mk(tab, cnt, i, n) { if (i >= n) { 'ok' } else { spawn(worker(i, tab, cnt, n))  mk(tab, cnt, i + 1, n) } }
fun each(tab, i, n, m) { if (i >= n) { 'ok' } else { send(ets_get(tab, i), m)  each(tab, i + 1, n, m) } }
fun main() {
    n = to_int(getenv("N"))
    tab = ets_new()
    cnt = ets_new()
    ets_put(cnt, 'hops', 0)
    mk(tab, cnt, 0, n)
    sleep(500)
    each(tab, 0, n, {'go'})
    each(tab, 0, n, {'tok', 1})
    sleep(to_int(getenv("HOLD_MS")))
    print("hops=" ++ to_string(ets_get(cnt, 'hops') * 1000))
    sys_exit(0)
}
