# Watchdog false-positive regression fixture.
#
# A lone process in `receive ... after` is waiting on its own timer, which
# will wake it; that is not a deadlock. The watchdog used to warn here, which
# was also the fix its own message recommends ("give its receive a timeout").
# It now stays silent while any timer is pending.

module Watchdog_timed_receive
export [main]

fun main() {
    r = receive { {'never'} -> 'got' after 1500 -> 'timeout' }
    print(to_string(r))
}
