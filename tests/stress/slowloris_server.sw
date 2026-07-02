# slowloris_server.sw — minimal HTTP/WS server for the slow-loris gate
# (tests/stress/slowloris_gate.sh). Listens on 9351, answers every
# http_request with 200 "ok", accepts WS upgrades and ignores them (an
# established-but-quiet WS session — must SURVIVE the idle sweep by default).
# Runs until the gate script kills it.
module Main

fun handler_loop() {
    receive {
        {'http_request', conn, _m, _p, _h, _b} -> {
            http_respond(conn, 200, "Content-Type: text/plain\r\n", "ok")
            handler_loop()
        }
        {'ws_connect', _conn, _path} -> handler_loop()
        _other -> handler_loop()
    }
}

fun main() {
    http_listen(9351)
    print("SLOWLORIS_SERVER_UP")
    handler_loop()
}
