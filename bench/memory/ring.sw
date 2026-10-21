# Per-process memory benchmark: the ring from skyblanket/swarmrt#161.
# Measure with bench/memory/measure_smaps.py (Linux); see docs/design/PROCESS_WEIGHT.md.
module Main
# N processes in a ring; every process forwards each token it gets to the next.
# MODE=idle: N parked processes.  MODE=busy: N processes passing tokens.
fun worker(i, tab, n) {
    ets_put(tab + i % 16, i, self())
    receive { {'go'} -> wloop(ets_get(tab + (i + 1) % n % 16, (i + 1) % n)) }
}
fun wloop(next) {
    receive { {'tok', k} -> send(next, {'tok', k + 1})  wloop(next)  _o -> wloop(next) }
}
fun idle() { receive { _o -> idle() } }
fun mk(tab, i, n) { if (i >= n) { 'ok' } else { spawn(worker(i, tab, n))  mk(tab, i + 1, n) } }
fun mkidle(i, n) { if (i >= n) { 'ok' } else { spawn(idle())  mkidle(i + 1, n) } }
fun tabs(k) { if (k <= 0) { 'ok' } else { ets_new()  tabs(k - 1) } }
fun go(tab, i, n) { if (i >= n) { 'ok' } else { send(ets_get(tab + i % 16, i), {'go'})  go(tab, i + 1, n) } }
fun kick(tab, i, n) { if (i >= n) { 'ok' } else { send(ets_get(tab + i % 16, i), {'tok', 0})  kick(tab, i + 1, n) } }
fun getenv_or(k, d) { v = getenv(k)  if (v == nil) { d } else { v } }
fun main() {
    n = to_int(getenv_or("N", "10000"))
    if (getenv_or("MODE", "busy") == "idle") { mkidle(0, n) }
    else {
        base = ets_new()
        tabs(15)
        mk(base, 0, n)
        sleep(500)
        go(base, 0, n)
        kick(base, 0, n)
        kick(base, 0, n)
    }
    sleep(12000)
}
