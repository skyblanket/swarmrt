# http_keepalive_slow_server.sw — echo server that answers each POST about
# 20 ms late (tests/stress/http_keepalive_gate.sh, phase 2). With 50 clients
# queued on one handler, a reply is owed for longer than the idle timeout the
# gate sets, which is the window the idle sweep used to close. Listens on
# 9352. Runs until the gate script kills it.
module Main

fun handler_loop() {
    receive {
        {'http_request', conn, _m, _p, _h, body} -> {
            receive { {'__never_sent__'} -> 0 after 20 -> 0 }
            http_respond(conn, 200, "Content-Type: text/plain\r\n", body)
            handler_loop()
        }
        _other -> handler_loop()
    }
}

fun main() {
    http_listen(9352)
    print("KEEPALIVE_SLOW_SERVER_UP")
    handler_loop()
}
