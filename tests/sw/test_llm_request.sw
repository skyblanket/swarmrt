module Test_llm_request

# llm_request(prompt, opts) returns a structured result, and its retry policy
# follows the server: a Retry-After is waited out exactly, a 400 is never
# retried, and a caller's timeout_ms bounds the whole call. A local server
# (below) plays each case by URL path; llm_request is pointed at it with
# opts.url, so no real endpoint is needed.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun assert_true(name, cond) { assert_eq(name, cond, 'true') }

fun listen_and_serve(port) {
    http_listen(port)
    serve(map_new())
}

# Pick a reply by path. `counts` is the per-path request count, so "/rl"
# answers 429 with Retry-After: 1 twice and then 200.
fun serve(counts) {
    receive {
        {'http_request', conn, _method, path, _headers, _body} ->
            seen = case map_get(counts, path) { 'nil' -> 0 ; v -> v }
            reply(conn, path, seen)
            serve(map_put(counts, path, seen + 1))
        _other -> serve(counts)
    }
}

fun reply(conn, path, seen) {
    if (path == "/rl" && seen < 2) {
        http_respond(conn, 429, "Retry-After: 1\r\nContent-Type: application/json\r\n",
                     "{\"error\":{\"type\":\"rate_limit_error\"}}")
    } else if (path == "/always429") {
        http_respond(conn, 429, "Retry-After: 60\r\nContent-Type: application/json\r\n",
                     "{\"error\":{\"type\":\"rate_limit_error\"}}")
    } else if (path == "/hang") {
        spawn(delayed_reply(conn, 3000))
    } else if (path == "/bad") {
        http_respond(conn, 400, "Content-Type: application/json\r\n",
                     "{\"error\":{\"message\":\"bad field\"}}")
    } else {
        http_respond(conn, 200, "Content-Type: application/json\r\n",
                     "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello there\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":2}}")
    }
}

fun delayed_reply(conn, ms) {
    sleep(ms)
    http_respond(conn, 200, "Content-Type: application/json\r\n", "{}")
}

# The telemetry entry for one endpoint URL, or 'nil'.
fun endpoint_stat(lst, url) {
    if (length(lst) == 0) { 'nil' }
    else {
        e = hd(lst)
        if (map_get(e, 'url') == url) { e } else { endpoint_stat(tl(lst), url) }
    }
}

fun main() {
    spawn(listen_and_serve(9421))
    sleep(300)
    base = "http://127.0.0.1:9421"
    failures = 0

    # --- 1. 429 + Retry-After: 1, twice, then 200: ok after ~2 s, 3 attempts.
    t0 = timestamp()
    r = llm_request("hi", %{url: base ++ "/rl", api_key: "k", retries: 3, backoff_ms: 100})
    el = timestamp() - t0
    case r {
        {'ok', m} ->
            failures = failures + assert_eq("rl_attempts", map_get(m, 'attempts'), 3)
            failures = failures + assert_eq("rl_content", map_get(m, 'content'), "hello there")
            failures = failures + assert_eq("rl_finish", map_get(m, 'finish_reason'), "stop")
            failures = failures + assert_eq("rl_prompt_tokens", map_get(map_get(m, 'usage'), 'prompt_tokens'), 5)
        _ ->
            print("FAIL rl_ok: got " ++ to_string(r))
            failures = failures + 1
    }
    # Two Retry-After waits of 1 s each: ~2 s, far from the 5 s + 15 s backoff.
    failures = failures + assert_true("rl_retry_after_honoured", el >= 1900 && el < 4500)

    # --- 2. 400 comes back at once as bad_request, with its body, unretried.
    t1 = timestamp()
    r2 = llm_request("x", %{url: base ++ "/bad", api_key: "k", retries: 3, backoff_ms: 100})
    el2 = timestamp() - t1
    case r2 {
        {'error', m} ->
            failures = failures + assert_eq("bad_kind", map_get(m, 'kind'), 'bad_request')
            failures = failures + assert_eq("bad_status", map_get(m, 'status'), 400)
            failures = failures + assert_eq("bad_attempts", map_get(m, 'attempts'), 1)
            failures = failures + assert_true("bad_body", string_contains(map_get(m, 'body'), "bad field") == 'true')
        _ ->
            print("FAIL bad_request: got " ++ to_string(r2))
            failures = failures + 1
    }
    failures = failures + assert_true("bad_not_retried", el2 < 1000)

    # --- 3. timeout_ms bounds the call: a 60 s Retry-After is not waited out.
    t2 = timestamp()
    r3 = llm_request("x", %{url: base ++ "/always429", api_key: "k", retries: 5, timeout_ms: 1500})
    el3 = timestamp() - t2
    case r3 {
        {'error', m} ->
            failures = failures + assert_eq("deadline_kind", map_get(m, 'kind'), 'rate_limited')
            failures = failures + assert_eq("deadline_retry_after", map_get(m, 'retry_after_ms'), 60000)
            failures = failures + assert_eq("deadline_attempts", map_get(m, 'attempts'), 1)
        _ ->
            print("FAIL deadline: got " ++ to_string(r3))
            failures = failures + 1
    }
    failures = failures + assert_true("deadline_no_long_wait", el3 < 5000)

    # --- 4. A reply that stalls past timeout_ms: the attempt itself is cut off
    # at the budget (curl --max-time), so the call is a timeout in about a
    # second, not after the 3 s stall.
    t3 = timestamp()
    r5 = llm_request("x", %{url: base ++ "/hang", api_key: "k", retries: 0, timeout_ms: 1000})
    el5 = timestamp() - t3
    case r5 {
        {'error', m} ->
            failures = failures + assert_eq("hang_kind", map_get(m, 'kind'), 'timeout')
        _ ->
            print("FAIL hang: got " ++ to_string(r5))
            failures = failures + 1
    }
    failures = failures + assert_true("hang_cut_at_budget", el5 < 2500)

    # --- 5. Nothing listening: a transport error, retried, then reported.
    r4 = llm_request("x", %{url: "http://127.0.0.1:9499/none", api_key: "k", retries: 1, backoff_ms: 50})
    case r4 {
        {'error', m} ->
            failures = failures + assert_eq("transport_kind", map_get(m, 'kind'), 'transport')
            failures = failures + assert_eq("transport_attempts", map_get(m, 'attempts'), 2)
            failures = failures + assert_eq("transport_status", map_get(m, 'status'), 'nil')
        _ ->
            print("FAIL transport: got " ++ to_string(r4))
            failures = failures + 1
    }

    # --- 6. The limiter's telemetry shows what happened on the /rl endpoint.
    stats = swarm_stats()
    rl = endpoint_stat(map_get(stats, 'llm_endpoints'), base ++ "/rl")
    failures = failures + assert_eq("stats_rl_requests", map_get(rl, 'requests'), 3)
    failures = failures + assert_eq("stats_rl_rate_limited", map_get(rl, 'rate_limited'), 2)
    failures = failures + assert_eq("stats_rl_retries", map_get(rl, 'retries'), 2)
    failures = failures + assert_eq("stats_rl_ok", map_get(rl, 'ok'), 1)

    if (failures == 0) { print("OK test_llm_request 23/23") ; sys_exit(0) }
    else { print(f"FAIL test_llm_request {failures} failed") ; sys_exit(1) }
}
