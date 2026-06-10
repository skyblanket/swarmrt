module Test_http_request

# Regression: http_request(url, opts) MUST surface the HTTP status code, the
# response headers, AND the body — as a tagged map %{status, body, headers}.
#
# Why this exists: the older http_post/http_get builtins return body-or-nil and
# hide the HTTP status entirely. The voice-agent port (Telnyx Call Control REST,
# see voice-agent/docs/RUNBOOK.md "No HTTP status from http_post" + the gap
# table) could not tell a 200 from a 4xx/5xx that carries a body — it had to
# guess from the JSON envelope shape ({"data":...} == success). http_request
# closes that gap by wiring curl's %{http_code} + the response headers (-D)
# through to the caller, WITHOUT changing http_post/http_get (still in use).
#
# This stands up a localhost HTTP server whose handler returns a deliberately
# NON-200 status (418), a custom response header (X-Echo: pong), and a known
# body. We assert http_request surfaces all three:
#   - status  == 418         (NOT hidden as it would be by http_post/http_get)
#   - headers["x-echo"] == "pong"   (response headers, lowercased keys)
#   - body    == "teapot-body"
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

# The HTTP handler: reply 418 with a custom RESPONSE header + a body that echoes
# the REQUEST method and the request's authorization header. This lets the
# client prove, in one round trip: (a) the status code surfaces, (b) a response
# header surfaces, (c) the body surfaces, AND (d) the opts-supplied request
# method + headers actually reached the server.
fun teapot_server() {
    http_listen(9137)
    teapot_loop()
}

fun teapot_loop() {
    receive {
        {'http_request', conn, method, _path, headers, _body} ->
            auth = case map_get(headers, "authorization") { 'nil' -> "<none>" v -> v }
            # body = "<METHOD>|<authorization-the-client-sent>"
            http_respond(conn, 418, "X-Echo: pong\r\nContent-Type: text/plain\r\n",
                         to_string(method) ++ "|" ++ auth)
            teapot_loop()
        _other ->
            teapot_loop()
    }
}

# http_request returns {'error', _} on a refused connection (server not bound
# yet), so retry until the spawned listener is up (same race the other loopback
# tests document).
fun req_retry(url, opts, attempts) {
    r = http_request(url, opts)
    case r {
        {'error', _reason} ->
            if (attempts <= 1) { r }
            else { sleep(50) req_retry(url, opts, attempts - 1) }
        _ -> r
    }
}

# Assert all four surfaced fields on a response map for a given expected body.
fun check_resp(resp, label, expected_body) {
    case resp {
        {'error', reason} ->
            print("FAIL " ++ label ++ "_connect: got error " ++ to_string(reason))
            1
        _ ->
            status  = map_get(resp, "status")
            body    = map_get(resp, "body")
            headers = map_get(resp, "headers")
            xecho   = case map_get(headers, "x-echo") { 'nil' -> "<none>" v -> v }
            f = 0
            f = f + assert_eq(label ++ "_status", status, 418)
            f = f + assert_eq(label ++ "_resp_header", xecho, "pong")
            f = f + assert_eq(label ++ "_body", body, expected_body)
            f
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
        print("OK test_http_request 0/0 (SKIP: needs >=3 schedulers — blocking client + in-process server)")
        sys_exit(0)
    }
    spawn(fun() { teapot_server() })

    fails = 0

    # (1) GET, headers supplied as a MAP (%{name => value}). The server echoes
    #     POST/GET + the authorization it received → proves method + map-headers
    #     flowed through opts, AND status/resp-header/body all surface.
    opts_map = map_put(
        map_put(map_new(), "method", "GET"),
        "headers", map_put(map_new(), "Authorization", "Bearer get-tok"))
    r1 = req_retry("http://127.0.0.1:9137/brew", opts_map, 100)
    fails = fails + check_resp(r1, "http_request_map", "GET|Bearer get-tok")

    # (2) POST, headers supplied as a LIST of {name, value} tuples + a body.
    #     Proves the other opts header shape + method override also reach the
    #     server (the list form is what http_get/http_post already accept).
    opts_list = map_put(
        map_put(
            map_put(map_new(), "method", "POST"),
            "headers", [{"Authorization", "Bearer post-tok"}]),
        "body", "ignored-body")
    r2 = req_retry("http://127.0.0.1:9137/brew", opts_list, 100)
    fails = fails + check_resp(r2, "http_request_list", "POST|Bearer post-tok")

    if (fails == 0) { print("OK test_http_request 6/6") sys_exit(0) }
    else { print(f"FAIL test_http_request {fails} failed") sys_exit(1) }
}
