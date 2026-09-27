# Watchdog false-positive regression fixture.
#
# exec_argv (like http_*, llm_complete) runs on the offload pool while the
# caller parks with an empty mailbox. The worker thread will wake it, so a
# slow call is not a deadlock; every LLM request longer than the watchdog
# interval used to print the warning.

module Watchdog_slow_offload
export [main]

fun main() {
    r = exec_argv("sleep", ["1.5"])
    print(to_string(elem(r, 0)))
}
