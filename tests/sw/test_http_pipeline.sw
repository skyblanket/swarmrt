module Test_http_pipeline

# Regression: pipelined HTTP requests. A client that sends two requests in ONE
# write() must get BOTH responses without sending any more bytes. The parser
# used to deliver one request per incoming chunk, so the second request sat in
# the connection buffer until the client wrote again (a stall, and with a
# client that waits for its responses, a deadlock).
#
# The raw socket client is bash's /dev/tcp: it writes both requests in one
# printf, then reads for up to 3s WITHOUT sending anything else. The server
# answers each request with its path as the body, so the output must hold
# both bodies in order. Exits non-zero on any failure so the driver rolls it up.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun pipeline_server() {
    http_listen(9471)
    pipeline_loop()
}

fun pipeline_loop() {
    receive {
        {'http_request', conn, _method, path, _headers, _body} ->
            http_respond(conn, 200, "Content-Type: text/plain\r\n", "ok:" ++ path)
            pipeline_loop()
        _other ->
            pipeline_loop()
    }
}

# Both requests in one write; the bodies come back on the same socket.
fun pipelined_bodies() {
    reqs = "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n"
    cmd = "bash -c \"exec 3<>/dev/tcp/127.0.0.1/9471; printf '%s' '" ++ reqs ++
          "' >&3; timeout 3 cat <&3 | grep -o 'ok:/[ab]'\""
    shell(cmd)
}

fun main() {
    spawn(fun() { pipeline_server() })
    sleep(300)

    out = elem(pipelined_bodies(), 1)
    f = 0
    f = f + assert_eq("pipelined_both_responses", out, "ok:/a\nok:/b\n")

    if (f == 0) { print("OK test_http_pipeline 1/1") sys_exit(0) }
    else { print(f"FAIL test_http_pipeline {f} failed") sys_exit(1) }
}
