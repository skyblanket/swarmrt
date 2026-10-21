# http_keepalive_server.sw — echo server for the keep-alive response gate
# (tests/stress/http_keepalive_gate.sh). Listens on 9352 and answers every
# POST with its own body, at once. Runs until the gate script kills it.
module Main

fun handler_loop() {
    receive {
        {'http_request', conn, _m, _p, _h, body} -> {
            http_respond(conn, 200, "Content-Type: text/plain\r\n", body)
            handler_loop()
        }
        _other -> handler_loop()
    }
}

fun main() {
    http_listen(9352)
    print("KEEPALIVE_SERVER_UP")
    handler_loop()
}
