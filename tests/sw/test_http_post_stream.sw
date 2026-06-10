module Test_http_post_stream

# End-to-end proof for http_post_stream's TAGGED return + SSE delta parsing,
# with NO live LLM credentials. A swarmrt HTTP server (http_listen) plays the
# role of an OpenAI-compatible /v1/chat/completions endpoint and replies with
# a canonical Server-Sent-Events body. The client (http_post_stream) parses
# the stream, accumulates delta.content, and returns {'ok, json} | {'error, why}.
#
# Covers the two regressions this batch fixed:
#   1. Tagged return — success yields {'ok, "<openai-json>"}, transport/HTTP/
#      empty failures yield {'error, "<reason>"} so the agent can branch.
#   2. The "data:{...}" (no space after colon) drop — the SSE spec makes the
#      space optional; the parser used to hard-require "data: " and silently
#      returned empty content from spec-legal servers.
#
# Exits non-zero on any failure so run_tests.sh rolls it up.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# Canonical OpenAI streaming SSE body WITH a space after "data:".
fun sse_with_space() {
    "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n" ++
    "data: {\"choices\":[{\"delta\":{\"content\":\" there\"}}]}\n\n" ++
    "data: [DONE]\n\n"
}

# Spec-legal SSE body WITHOUT a space after "data:" — the bug case.
fun sse_no_space() {
    "data:{\"choices\":[{\"delta\":{\"content\":\"no\"}}]}\n\n" ++
    "data:{\"choices\":[{\"delta\":{\"content\":\" space\"}}]}\n\n" ++
    "data:[DONE]\n\n"
}

# Server: route by path so one listener can serve several SSE fixtures and a
# 404 case in one test run.
fun serve(port) {
    http_listen(port)
    serve_loop()
}

fun serve_loop() {
    receive {
        {'http_request', conn, _method, path, _headers, _body} ->
            sse_hdr = "Content-Type: text/event-stream\r\n"
            case path {
                "/space"    -> http_respond(conn, 200, sse_hdr, sse_with_space())
                "/nospace"  -> http_respond(conn, 200, sse_hdr, sse_no_space())
                "/notfound" -> http_respond(conn, 404,
                                  "Content-Type: application/json\r\n",
                                  "{\"error\":{\"message\":\"model not found\"}}")
                _           -> http_respond(conn, 200, sse_hdr, sse_with_space())
            }
            serve_loop()
    }
}

# The first request can race the listener's bind(), so retry with backoff.
# On success http_post_stream returns {'ok, _} or {'error, _}; on a refused
# connection it returns {'error, "curl exit 7: ..."}. We retry only while
# the error reason looks like a connection refusal.
fun post_retry(url, body, attempts) {
    r = http_post_stream(url, [{"Content-Type", "application/json"}], body)
    case r {
        {'error', why} ->
            if (attempts > 1 && string_index_of(why, "Connection refused") >= 0) {
                sleep(50)
                post_retry(url, body, attempts - 1)
            } else { r }
        _ -> r
    }
}

# Pull choices[0].message.content out of an {'ok, json} result.
fun content_of(json) {
    decoded = json_decode(json)
    choices = map_get(decoded, 'choices')
    map_get(map_get(hd(choices), 'message'), 'content')
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
        print("OK test_http_post_stream 0/0 (SKIP: needs >=3 schedulers — blocking client + in-process server)")
        sys_exit(0)
    }
    port = 9131
    base = f"http://127.0.0.1:{port}"
    body = json_encode(%{model: "stub", stream: 'true', messages: []})

    spawn(fun() { serve(port) })
    fails = 0

    # 1. "data: " (with space) — success, content accumulates across deltas.
    r1 = post_retry(f"{base}/space", body, 100)
    case r1 {
        {'ok', j1} ->
            fails = fails + assert_eq("space_content", content_of(j1), "hi there")
        {'error', w1} ->
            print(f"FAIL space_ok: got error {w1}")
            fails = fails + 1
    }

    # 2. "data:" (no space) — the regression. Must ALSO succeed with content.
    r2 = post_retry(f"{base}/nospace", body, 100)
    case r2 {
        {'ok', j2} ->
            fails = fails + assert_eq("nospace_content", content_of(j2), "no space")
        {'error', w2} ->
            print(f"FAIL nospace_ok: got error {w2}")
            fails = fails + 1
    }

    # 3. Non-2xx HTTP status — must come back tagged {'error, "HTTP 404: ..."}.
    r3 = post_retry(f"{base}/notfound", body, 100)
    case r3 {
        {'ok', _j3} ->
            print("FAIL http404_should_error: got ok")
            fails = fails + 1
        {'error', w3} ->
            fails = fails + assert_eq("http404_tagged",
                                      string_index_of(w3, "HTTP 404") >= 0, 'true')
    }

    # 4. Connection refused on a dead port — tagged {'error, "curl exit ..."}.
    r4 = http_post_stream(f"http://127.0.0.1:9132/dead",
                          [{"Content-Type", "application/json"}], body)
    case r4 {
        {'ok', _j4} ->
            print("FAIL dead_port_should_error: got ok")
            fails = fails + 1
        {'error', w4} ->
            fails = fails + assert_eq("dead_port_tagged",
                                      string_index_of(w4, "curl exit") >= 0, 'true')
    }

    if (fails == 0) { print("OK test_http_post_stream 4/4") ; sys_exit(0) }
    else { print(f"FAIL test_http_post_stream {fails} failed") ; sys_exit(1) }
}
