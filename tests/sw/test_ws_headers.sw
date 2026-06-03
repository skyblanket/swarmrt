module Test_ws_headers

# Regression: a WebSocket UPGRADE request's headers (and path) MUST be
# reachable by the sw handler. Before the fix, src/swarmrt_http.c exposed
# nothing for a WS connection — so Origin/CSRF checks and Telnyx webhook-
# signature reads on the media socket were impossible (voice-agent/Telnyx.sw
# could only run signature verify in dev mode, see the RUNBOOK).
#
# The fix keeps {'ws_connect', conn, path} as a 3-tuple (backward compatible
# with studio/swarm-live LiveView handlers) and adds two query builtins:
#   ws_request_headers(conn) -> MAP (lowercased keys) from the UPGRADE request
#   ws_request_path(conn)    -> the UPGRADE request's path+query
#
# This stands up a WS server; on the first {'ws_message', conn, _} frame its
# handler reads ws_request_headers(conn) / ws_request_path(conn) (the headers
# the socket was UPGRADED with — they live for the connection's whole life,
# not just the {'ws_connect'} instant) and ws_sends them back as one line.
# wsc_connect's fixed handshake carries `Upgrade: websocket` and
# `Sec-WebSocket-Version: 13` and the GET path verbatim, so we assert all
# three round-trip.
#
# Why the client sends a frame first (then recvs): this exactly mirrors the
# proven-stable test_voice_ws_loopback.sw ordering. An in-VM wsc_connect does
# a *blocking* handshake on the scheduler thread; pumping a client send before
# the recv keeps the scheduler driving both sides (a documented runtime
# asymmetry — see the RUNBOOK's "in-VM blocking WS connect" note). For the
# same reason this is a separate file/VM from test_http_headers.sw: a live
# HTTP server connection in the same VM would starve the WS handshake.
#
# Exits non-zero on any failure so the driver rolls it up.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun ws_header_server() {
    http_listen(9132)
    ws_header_loop()
}

fun ws_header_loop() {
    receive {
        # The client sends "ready" once connected; reply with the headers +
        # path captured from the UPGRADE request (still queryable here).
        {'ws_message', conn, _text} ->
            hdrs = ws_request_headers(conn)
            up   = case map_get(hdrs, "upgrade") { 'nil' -> "<none>" v -> v }
            ver  = case map_get(hdrs, "sec-websocket-version") { 'nil' -> "<none>" v -> v }
            ws_send(conn, up ++ "|" ++ ver ++ "|" ++ ws_request_path(conn))
            ws_header_loop()
        {'ws_connect', _conn, _path} ->
            ws_header_loop()
        {'ws_close', _conn} ->
            ws_header_loop()
        _other ->
            ws_header_loop()
    }
}

fun ws_connect_retry(url, attempts) {
    h = wsc_connect(url)
    case h {
        'nil' ->
            if (attempts <= 1) { 'nil' }
            else { sleep(50) ws_connect_retry(url, attempts - 1) }
        _ -> h
    }
}

fun main() {
    spawn(fun() { ws_header_server() })

    wh = ws_connect_retry("ws://127.0.0.1:9132/telnyx/media", 100)
    fails = 0
    case wh {
        'nil' ->
            print("FAIL ws_headers_connect: wsc_connect returned nil")
            fails = 1
        _ ->
            wsc_send(wh, "ready")    # drive the scheduler past the blocking connect
            echo = wsc_recv(wh, 3000)
            fails = fails + assert_eq("ws_request_headers_roundtrip", echo,
                                      "websocket|13|/telnyx/media")
            wsc_close(wh)
    }

    if (fails == 0) { print("OK test_ws_headers 1/1") sys_exit(0) }
    else { print(f"FAIL test_ws_headers {fails} failed") sys_exit(1) }
}
