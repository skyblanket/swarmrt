module Test_llm_limit

# llm_limit(url, %{rpm:, max_concurrency:}) is a per-endpoint limiter shared by
# every process: callers that exceed it wait, rather than stacking requests on
# the server. Two checks against a local server: a 600-rpm pace under 60
# concurrent callers, and a concurrency cap of 2 against a slow endpoint.

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
    serve()
}

fun serve() {
    receive {
        {'http_request', conn, _method, path, _headers, _body} ->
            if (path == "/slow") { spawn(delayed_reply(conn, 500)) }
            else { reply(conn) }
            serve()
        _other -> serve()
    }
}

fun reply(conn) {
    http_respond(conn, 200, "Content-Type: application/json\r\n",
                 "{\"choices\":[{\"message\":{\"content\":\"pong\"},\"finish_reason\":\"stop\"}]}")
}

fun delayed_reply(conn, ms) {
    sleep(ms)
    reply(conn)
}

# One caller: one llm_request, and its outcome goes back to the parent.
fun caller(parent, url) {
    send(parent, {'done', llm_request("ping", %{url: url, api_key: "k", retries: 0})})
}

fun spawn_callers(i, n, parent, url) {
    if (i == n) { 'ok' }
    else { spawn(caller(parent, url)) ; spawn_callers(i + 1, n, parent, url) }
}

# Collect n outcomes; returns how many were {'ok', _}.
fun collect(n, oks) {
    if (n == 0) { oks }
    else {
        receive {
            {'done', {'ok', _}} -> collect(n - 1, oks + 1)
            {'done', _} -> collect(n - 1, oks)
        }
    }
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
    spawn(listen_and_serve(9422))
    sleep(300)
    base = "http://127.0.0.1:9422"
    failures = 0

    # --- 1. rpm paces request starts 60000/rpm ms apart, with no burst, however
    # many callers pile up. The issue's case is rpm 60 with 120 callers (~2 min);
    # this is the same shape at 10x the rate so the suite stays fast: rpm 600,
    # 60 concurrent callers, so the last start is ~59 x 100 ms after the first.
    paced = base ++ "/paced"
    llm_limit(paced, %{rpm: 600})
    t0 = timestamp()
    spawn_callers(0, 60, self(), paced)
    oks = collect(60, 0)
    el = timestamp() - t0
    failures = failures + assert_eq("paced_all_ok", oks, 60)
    failures = failures + assert_true("paced_elapsed", el >= 5500)
    # ...and no slower than the pace demands (a lost wakeup would show here).
    failures = failures + assert_true("paced_not_stalled", el < 20000)
    stats = swarm_stats()
    p = endpoint_stat(map_get(stats, 'llm_endpoints'), paced)
    failures = failures + assert_eq("paced_requests", map_get(p, 'requests'), 60)
    failures = failures + assert_eq("paced_in_flight_drained", map_get(p, 'in_flight'), 0)

    # --- 2. max_concurrency: 2 holds 6 callers to three waves of 500 ms.
    slow = base ++ "/slow"
    llm_limit(slow, %{max_concurrency: 2})
    t1 = timestamp()
    spawn_callers(0, 6, self(), slow)
    slow_oks = collect(6, 0)
    el2 = timestamp() - t1
    failures = failures + assert_eq("slow_all_ok", slow_oks, 6)
    failures = failures + assert_true("slow_waves", el2 >= 1400)

    if (failures == 0) { print("OK test_llm_limit 7/7") ; sys_exit(0) }
    else { print(f"FAIL test_llm_limit {failures} failed") ; sys_exit(1) }
}
