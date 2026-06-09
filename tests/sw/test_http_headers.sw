module Test_http_headers

# Regression: the HTTP server MUST deliver the request headers to the sw
# handler. Before the fix, src/swarmrt_http.c passed headers="" in the
# {'http_request', ...} tuple — so a bearer-token check or a webhook-
# signature read was impossible in-process (the voice-agent port had to fall
# back to a `token` body field, see voice-agent/docs/RUNBOOK.md).
#
# This stands up a localhost HTTP server whose handler reads `authorization`
# and a custom `x-test-header` out of the delivered headers MAP and echoes
# them back. A client (http_get, which shells curl in its own process) sends
# those headers; we assert the exact bytes round-trip — proving headers reach
# the handler with lowercased keys.
#
# (The WS-upgrade header path is covered by test_ws_headers.sw. It lives in a
# separate file/VM on purpose: an in-VM wsc_connect deadlocks against a live
# HTTP server connection — a documented runtime limitation, see the RUNBOOK.)
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

# The HTTP handler: read two headers out of the delivered MAP and respond
# with "<authorization>|<x-test-header>" so the client can verify the exact
# bytes survived. The server lowercases header NAMES, so we look them up
# lowercased (the VALUE casing is preserved).
fun header_server() {
    http_listen(9131)
    header_loop()
}

fun header_loop() {
    receive {
        {'http_request', conn, _method, _path, headers, _body} ->
            auth = case map_get(headers, "authorization") { 'nil' -> "<none>" v -> v }
            xt   = case map_get(headers, "x-test-header") { 'nil' -> "<none>" v -> v }
            http_respond(conn, 200, "Content-Type: text/plain\r\n", auth ++ "|" ++ xt)
            header_loop()
        _other ->
            header_loop()
    }
}

# http_get returns nil on a refused connection, so retry until the spawned
# listener has bound (same race the ws loopback test documents).
fun get_retry(url, hdrs, attempts) {
    r = http_get(url, hdrs)
    case r {
        'nil' ->
            if (attempts <= 1) { 'nil' }
            else { sleep(50) get_retry(url, hdrs, attempts - 1) }
        _ -> r
    }
}

fun main() {
    # Self-loopback test: an in-process HTTP server fiber + a BLOCKING
    # curl-backed client on the same runtime. The client call occupies its
    # scheduler OS THREAD (not just the fiber), so with one scheduler the
    # server can never run -> deadlock/timeout (found by the Phase-2.2
    # scheduler matrix). Needs >=2 schedulers until blocking builtins are
    # moved off the scheduler thread (see KNOWN_ISSUES).
    nsched = getenv("SW_SCHEDULERS")
    if (nsched == "1" || nsched == "2") {
        print("OK test_http_headers 0/0 (SKIP: needs >=3 schedulers — blocking client + in-process server)")
        sys_exit(0)
    }
    spawn(fun() { header_server() })

    # Bearer token + a custom header. Only the header NAME is lowercased by
    # the server; the VALUE (mixed case) must come back intact.
    bearer = "Bearer secret-round-trip-token-42"
    custom = "telnyx-sig-abc123"
    hdrs = [{"Authorization", bearer}, {"X-Test-Header", custom}]

    body = get_retry("http://127.0.0.1:9131/auth", hdrs, 100)
    fails = 0
    case body {
        'nil' ->
            print("FAIL http_headers_connect: http_get returned nil")
            fails = 1
        _ ->
            expected = bearer ++ "|" ++ custom
            fails = fails + assert_eq("http_headers_roundtrip", body, expected)
    }

    if (fails == 0) { print("OK test_http_headers 1/1") sys_exit(0) }
    else { print(f"FAIL test_http_headers {fails} failed") sys_exit(1) }
}
