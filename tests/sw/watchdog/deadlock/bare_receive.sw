# The watchdog must still report a real deadlock: the only process waits in
# a bare `receive` (no timer, no port, no offload job), so nothing can ever
# wake it. run_tests.sh expects the "possible deadlock" warning on stderr.

module Watchdog_bare_receive
export [main]

fun main() {
    receive { {'never'} -> 'got' }
}
