# Watchdog false-positive regression fixture.
#
# Back-to-back offloaded calls with some work in between. The watchdog
# reads process states first and its wake sources (the parked-offload
# count, timers, ports) after; a call finishing between those reads used
# to look like every process blocked with nothing to wake it. It must now
# see a whole interval with nothing dispatched before it warns.

module Watchdog_offload_loop
export [main]

fun work(i, n, acc) {
    if (i == n) { acc } else { work(i + 1, n, acc + i) }
}

fun run(deadline, calls) {
    if (timestamp() > deadline) { calls }
    else {
        exec_argv("true", [])
        work(0, 60000, 0)
        run(deadline, calls + 1)
    }
}

fun main() {
    calls = run(timestamp() + 3000, 0)
    print(f"offload calls: {calls}")
}
