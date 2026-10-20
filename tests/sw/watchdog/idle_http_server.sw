# Watchdog false-positive regression fixture.
#
# An idle TCP/HTTP server legitimately parks every live process in
# `receive` with an empty mailbox: the main loop and the http_listen
# bridge both wait for the I/O thread to deliver an accept/data event.
# Before the fix, the deadlock watchdog saw `stuck_count == live_count`
# and printed a spurious "possible deadlock" WARNING to stderr.
#
# The fix: the watchdog also checks sw_io_active_port_count() and stays
# silent while a live port can still wake a process.
#
# This fixture starts a server on an unused port, idles past the
# watchdog interval (the run_tests.sh wrapper sets SW_DEADLOCK_MS small),
# then exits cleanly via the receive `after` clause. The wrapper asserts
# that NO "possible deadlock" line appeared on stderr.

module Watchdog_idle_http_server
export [main]

# Listener port for this fixture. run_tests.sh exports SW_TEST_PORT_BASE per
# run so two suite runs on one host can't bind the same port; run by hand,
# the base falls back to 9000.
fun test_port(offset) {
    base = case getenv("SW_TEST_PORT_BASE") { 'nil' -> 9000 v -> to_int(v) }
    base + offset
}

fun main() {
    http_listen(test_port(180))
    print("LISTENING")
    serve()
}

fun serve() {
    receive {
        {'http_request', conn, _m, _p, _h, _b} ->
            http_respond(conn, 200, [], "ok\n")
            serve()
        after 900 {
            print("IDLE_DONE")
        }
    }
}
