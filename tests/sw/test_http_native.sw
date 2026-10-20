module Test_http_native

# sw-level check of the HTTP builtins with a 1 MB body each way. The same
# assertions run with curl (default) and with the native client
# (SW_HTTP_NATIVE=1): run_tests.sh inherits the environment, so
#   SW_HTTP_NATIVE=1 tests/sw/run_tests.sh
# exercises the plain-http:// native path for these requests.
#
# Chunked replies, a reply cut short and interrupts need a raw server, so they
# are covered by tests/native/test_http_native.c instead. The opts.timeout_ms
# check uses an in-VM route that never answers.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun check_true(name, ok, detail) {
    if (ok) { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name ++ ": " ++ detail) ; 1 }
}

# 1 MB built by doubling, so the payload is the same string every run.
fun big_string(s, times) {
    if (times <= 0) { s }
    else { big_string(s ++ s, times - 1) }
}

fun echo_server() {
    http_listen(9139)
    echo_loop()
}

# POST /echo answers with the request body; GET /big answers with 1 MB;
# GET /hang never answers.
fun echo_loop() {
    receive {
        {'http_request', conn, _method, path, _headers, body} ->
            if (path == "/big") {
                http_respond(conn, 200, "Content-Type: application/octet-stream\r\n",
                             big_string("x", 20))
            } else {
                if (path == "/hang") { 0 }
                else { http_respond(conn, 200, "Content-Type: application/octet-stream\r\n", body) }
            }
            echo_loop()
        _other ->
            echo_loop()
    }
}

# The listener is spawned, so the first call may race it: retry on an
# empty body (http_post) or an error (http_request).
fun post_retry(url, body, attempts) {
    r = http_post(url, [{"Content-Type", "application/octet-stream"}], body)
    if (r == "" && attempts > 1) { sleep(50) post_retry(url, body, attempts - 1) }
    else { r }
}

fun main() {
    # Blocking client + in-process server: needs >=3 schedulers (see test_http_request).
    nsched = getenv("SW_SCHEDULERS")
    if (nsched == "1" || nsched == "2") {
        print("OK test_http_native 0/0 (SKIP: needs >=3 schedulers — blocking client + in-process server)")
        sys_exit(0)
    }
    spawn(fun() { echo_server() })

    fails = 0
    payload = big_string("0123456789abcdef", 16)   # 16 * 2^16 = 1 MB
    n = string_length(payload)

    # (1) http_post: 1 MB request body, 1 MB echoed back.
    echoed = post_retry("http://127.0.0.1:9139/echo", payload, 100)
    fails = fails + check_true("post_1mb_length", string_length(echoed) == n,
                               "length " ++ to_string(string_length(echoed)))
    fails = fails + check_true("post_1mb_content", echoed == payload, "body differs")

    # (2) http_get: 1 MB response body.
    big = http_get("http://127.0.0.1:9139/big", [])
    fails = fails + check_true("get_1mb_length", string_length(big) == 1048576,
                               "length " ++ to_string(string_length(big)))

    # (3) http_request: status, 1 MB request body, 1 MB response body.
    r = http_request("http://127.0.0.1:9139/echo",
                     map_put(map_put(map_new(), "method", "POST"), "body", payload))
    case r {
        {'error', reason} ->
            fails = fails + check_true("request_1mb_connect", false, to_string(reason))
        _ ->
            fails = fails + assert_eq("request_1mb_status", map_get(r, "status"), 200)
            fails = fails + check_true("request_1mb_content", map_get(r, "body") == payload,
                                       "body differs")
    }

    # (4) http_request with opts.timeout_ms = 500 against a server that never
    # answers: an error, back in about the budget (curl: --max-time 1).
    t0 = timestamp()
    hung = http_request("http://127.0.0.1:9139/hang", map_put(map_new(), "timeout_ms", 500))
    dt = timestamp() - t0
    case hung {
        {'error', _reason} -> fails = fails + check_true("request_timeout_error", true, "")
        _ -> fails = fails + check_true("request_timeout_error", false, "no error returned")
    }
    fails = fails + check_true("request_timeout_fast", dt < 3000,
                               "took " ++ to_string(dt) ++ " ms")

    if (fails == 0) { print("OK test_http_native 7/7") sys_exit(0) }
    else { print(f"FAIL test_http_native {fails} failed") sys_exit(1) }
}
