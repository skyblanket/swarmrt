module Test_http_port_free

# Regression: the HTTP server must free a closed connection's sw_port_t.
# Closing a connection closed the socket but never freed the port struct
# (the IO thread may still hold it in an event batch fetched before the
# close), so every connection leaked ~100 bytes for the life of the
# process. Closed ports are now retired and freed by the IO thread between
# event batches. swarm_stats()'s io_ports (live port structs) must stay flat
# across connection churn on every close path:
#   - peer hang-up (a fresh curl per request, keep-alive, curl exits)
#   - server close (Connection: close request)
#   - WebSocket close from the handler side (ws_close)

import Std

fun server() {
    http_listen(9161)
    serve()
}

fun serve() {
    receive {
        {'http_request', conn, _m, _p, _h, _b} -> http_respond(conn, 200, "", "ok") ; serve()
        {'ws_message', conn, _t} -> ws_close(conn) ; serve()
        _other -> serve()
    }
}

fun wait_up(attempts) {
    r = http_request("http://127.0.0.1:9161/ping", %{})
    case r {
        {'error', _} -> if (attempts <= 1) { 'down' } else { sleep(50) ; wait_up(attempts - 1) }
        _ -> 'up'
    }
}

fun hangups(i, n, ok) {
    if (i == n) { ok }
    else {
        body = http_get("http://127.0.0.1:9161/x")
        hangups(i + 1, n, if (body == "ok") { ok + 1 } else { ok })
    }
}

fun server_closes(i, n, ok) {
    if (i == n) { ok }
    else {
        r = http_request("http://127.0.0.1:9161/c", %{headers: %{"Connection" => "close"}})
        good = case r { {'error', _} -> false _ -> true }
        server_closes(i + 1, n, if (good) { ok + 1 } else { ok })
    }
}

fun ws_rounds(i, n, ok) {
    if (i == n) { ok }
    else {
        h = wsc_connect("ws://127.0.0.1:9161/ws")
        good = case h {
            'nil' -> false
            _ -> wsc_send(h, "bye") ; wsc_recv(h, 2000) ; wsc_close(h) ; true
        }
        ws_rounds(i + 1, n, if (good) { ok + 1 } else { ok })
    }
}

fun io_ports() {
    case map_get(swarm_stats(), 'io_ports') { 'nil' -> -1 v -> v }
}

fun main() {
    spawn(server())
    if (wait_up(100) != 'up') { print("FAIL server_up") ; sys_exit(1) }
    sleep(300)
    before = io_ports()
    fails = 0
    if (before >= 0) { print("PASS io_ports_reported") }
    else { print("FAIL io_ports_reported: swarm_stats() has no io_ports") ; fails = fails + 1 }

    a = hangups(0, 300, 0)
    b = server_closes(0, 300, 0)
    c = ws_rounds(0, 100, 0)
    sleep(500)
    later = io_ports()
    if (a == 300 && b == 300 && c == 100) { print("PASS all_ok") }
    else { print(f"FAIL all_ok: hangup {a}/300, close {b}/300, ws {c}/100") ; fails = fails + 1 }
    if (before >= 0 && later - before < 10) { print(f"PASS ports_freed ({before} -> {later} over 700 connections)") }
    else { print(f"FAIL ports_freed: io_ports {before} -> {later} over 700 connections") ; fails = fails + 1 }

    if (fails == 0) { print("OK http_port_free 3/3") ; sys_exit(0) }
    else { print("FAIL http_port_free") ; sys_exit(1) }
}
