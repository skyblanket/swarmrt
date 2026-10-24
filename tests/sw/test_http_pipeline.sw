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

# Listener ports for this test: a per-process base (derived from our pid, or
# SW_TEST_PORT_BASE when set) plus a fixed offset, so two suite runs on one
# host don't bind the same port.
fun test_base() {
    case getenv("SW_TEST_PORT_BASE") {
        'nil' -> case shell("printf %s $PPID") { {_c, out} -> 20000 + (to_int(out) * 97) % 40000 }
        v -> to_int(v)
    }
}

fun test_port(offset) {
    test_base() + offset
}

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun pipeline_server() {
    http_listen(test_port(471))
    pipeline_loop()
}

fun pipeline_loop() {
    receive {
        {'http_request', conn, _method, path, _headers, _body} ->
            if (path == "/slow") { sleep(300) } else { 0 }
            http_respond(conn, 200, "Content-Type: text/plain\r\n", "ok:" ++ path)
            pipeline_loop()
        _other ->
            pipeline_loop()
    }
}

# Both requests in one write; the bodies come back on the same socket.
fun pipelined_bodies() {
    pipelined_out("GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n")
}

# /a asks to close, so /b must not be served on that connection.
fun pipelined_close_bodies() {
    pipelined_out("GET /a HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" ++
                  "GET /b HTTP/1.1\r\nHost: x\r\n\r\n")
}

# Keep-alive /slow followed by a closing /b: /b must wait for /slow's response
# (not cut it off) and still be answered once it is out.

fun pipelined_slow_close_bodies_slow() {
    pipelined_out("GET /slow HTTP/1.1\r\nHost: x\r\n\r\n" ++
                  "GET /b HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
}

fun pipelined_out(reqs) {
    cmd = "bash -c \"exec 3<>/dev/tcp/127.0.0.1/" ++ to_string(test_port(471)) ++ ";  printf '%s' '" ++ reqs ++
          "' >&3; perl -e 'alarm 3; exec @ARGV' cat <&3 | grep -o 'ok:/[a-z]*'\""
    elem(shell(cmd), 1)
}

fun main() {
    spawn(fun() { pipeline_server() })
    sleep(300)

    out = pipelined_bodies()
    close_out = pipelined_close_bodies()
    slow_out = pipelined_slow_close_bodies_slow()
    f = 0
    f = f + assert_eq("pipelined_both_responses", out, "ok:/a\nok:/b\n")
    f = f + assert_eq("close_stops_pipeline", close_out, "ok:/a\n")

    f = f + assert_eq("held_close_after_slow", slow_out, "ok:/slow\nok:/b\n")

    if (f == 0) { print("OK test_http_pipeline 3/3") sys_exit(0) }
    else { print(f"FAIL test_http_pipeline {f} failed") sys_exit(1) }
}
