module Test_http_chunked

# Regression: a Transfer-Encoding: chunked request body used to be dropped.
# The server logged "not yet supported" and dispatched the handler with an
# empty body, so a client that streams its upload lost the whole payload.
#
# Now the body is decoded as it arrives. Checked here with curl, which sends
# the body chunked when given the header:
#   - a multi-chunk upload reaches the handler byte for byte;
#   - an empty chunked body reaches the handler as "" (no crash);
#   - a chunked body over SW_HTTP_MAX_REQUEST gets 413, like Content-Length;
#   - a long request path reaches the handler whole;
#   - ambiguous framing (CL+TE, unsupported or misplaced codings, conflicting
#     Content-Length, a bare CR in a chunk-size line) gets one 400 and a close.
#
# The body runs in a child with SW_HTTP_MAX_REQUEST=4096 (the minimum) and
# SW_SCHEDULERS=2: curl blocks a scheduler thread, and the listener needs the
# other one. Same shape as test_ws_origin.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# Echo the request body back, so the client can compare it byte for byte.
# A path over 100 bytes is answered with its length instead, to show it arrived whole.
fun serve_loop() {
    receive {
        {'http_request', conn, _method, path, _headers, body} ->
            reply = if (string_length(path) > 100) { to_string(string_length(path)) } else { body }
            http_respond(conn, 200, "Content-Type: text/plain\r\n", reply)
            serve_loop()
        _other -> serve_loop()
    }
}

fun serve(port) {
    http_listen(port)
    serve_loop()
}

# curl's stdout, trimmed. --retry-connrefused covers the listener's bind race.
fun curl_out(cmd) {
    r = shell(cmd)
    string_trim(elem(r, 1))
}

fun child() {
    url = "http://127.0.0.1:9172/chunked"
    curl = "curl -s --retry 20 --retry-connrefused --max-time 5 -H 'Transfer-Encoding: chunked' "
    spawn(fun() { serve(9172) })

    fails = 0
    # 1. ~2.9 KB of text: several chunks on the wire, compared with the file it came from.
    fails = fails + assert_eq("chunked_body_byte_exact",
        curl_out("f=$(mktemp); seq 1 700 > $f; " ++ curl ++ "--data-binary @$f " ++ url ++
                 " | cmp -s - $f && echo SAME || echo DIFF; rm -f $f"),
        "SAME")
    # 2. Zero-length chunked body: the handler gets "" and the server stays up.
    fails = fails + assert_eq("chunked_empty_body",
        curl_out(curl ++ "-d '' " ++ url ++ " | wc -c"),
        "0")
    # 3. ~14 KB > 4096: refused with 413 before the body is read.
    fails = fails + assert_eq("chunked_oversize_413",
        curl_out("f=$(mktemp); seq 1 3000 > $f; " ++ curl ++
                 "-o /dev/null -w '%{http_code}' --data-binary @$f " ++ url ++ "; rm -f $f"),
        "413")
    # 4. A 201-byte path must reach the handler whole (the body stash used to cut it at 128).
    fails = fails + assert_eq("chunked_long_path_intact",
        curl_out(curl ++ "-d 'x' http://127.0.0.1:9172/$(printf 'a%.0s' $(seq 1 200))"),
        "201")

    # 5-10. Framing curl can't send (tests/sw/http_raw.py: "<responses> <status>").
    # Ambiguous framing is a request-smuggling opening behind a proxy, so it
    # must get one 400 and a closed connection, never a second response.
    raw = "python3 \"$(dirname '" ++ hd(os_args()) ++ "')/../http_raw.py\" 9172 "
    pre = "POST /r HTTP/1.1\\r\\nHost: a\\r\\n"
    body = "\\r\\n\\r\\n3\\r\\nabc\\r\\n0\\r\\n\\r\\n"
    fails = fails + assert_eq("te_prefix_match_rejected",
        curl_out(raw ++ "'" ++ pre ++ "Transfer-Encoding: xchunked" ++ body ++ "'"), "1 400")
    fails = fails + assert_eq("te_chunked_not_last_rejected",
        curl_out(raw ++ "'" ++ pre ++ "Transfer-Encoding: chunked, gzip" ++ body ++ "'"), "1 400")
    fails = fails + assert_eq("te_gzip_chunked_rejected",
        curl_out(raw ++ "'" ++ pre ++ "Transfer-Encoding: gzip, chunked" ++ body ++ "'"), "1 400")
    fails = fails + assert_eq("te_trailing_space_ok",
        curl_out(raw ++ "'" ++ pre ++ "Connection: close\\r\\nTransfer-Encoding: chunked " ++ body ++ "'"), "1 200")
    fails = fails + assert_eq("cl_te_smuggle_rejected",
        curl_out(raw ++ "'" ++ pre ++ "Content-Length: 4\\r\\nTransfer-Encoding: chunked\\r\\n\\r\\n" ++
                 "0\\r\\n\\r\\nGET /smuggled HTTP/1.1\\r\\nHost: a\\r\\n\\r\\n'"), "1 400")
    fails = fails + assert_eq("cl_conflict_rejected",
        curl_out(raw ++ "'" ++ pre ++ "Content-Length: 3\\r\\nContent-Length: 4\\r\\n\\r\\nabcd'"), "1 400")
    fails = fails + assert_eq("chunk_size_bare_cr_rejected",
        curl_out(raw ++ "'" ++ pre ++ "Transfer-Encoding: chunked\\r\\n\\r\\n3\\rxy\\nabc\\r\\n0\\r\\n\\r\\n'"), "1 400")
    fails = fails + assert_eq("te_chunked_case_ok",
        curl_out(raw ++ "'" ++ pre ++ "Connection: close\\r\\nTransfer-Encoding: CHUNKED" ++ body ++ "'"), "1 200")
    sys_exit(fails)
}

fun main() {
    if (getenv("SW_HTTP_CHUNKED_CHILD") == "1") { child() }
    me = hd(os_args())
    r = shell("SW_HTTP_CHUNKED_CHILD=1 SW_SCHEDULERS=2 SW_HTTP_MAX_REQUEST=4096 '" ++ me ++ "' 2>/dev/null")
    print(string_trim(elem(r, 1)))
    if (elem(r, 0) == 0) { print("OK test_http_chunked 12/12") ; sys_exit(0) }
    else { print("FAIL test_http_chunked") ; sys_exit(1) }
}
