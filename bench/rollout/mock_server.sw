module Mock_server

# The mock model server in sw: the same contract as mock_server.py (see
# README.md), for dogfooding SwarmRT's own HTTP server under load. The Python
# mock stays the neutral default for comparisons, so a SwarmRT server bug
# can't skew SwarmRT's own numbers.
#
#   swc build mock_server.sw -o mock && PORT=18600 ./mock
#
# Latency is 50-150 ms, from a hash of (id, turn) rather than Python's seeded
# RNG: same range, different values.

import Std

fun env_int(name, dflt) {
    v = getenv(name)
    if (v == nil || v == "") { dflt } else { to_int(v) }
}

fun json_headers() { "Content-Type: application/json\r\n" }

fun chat_reply(content) {
    json_encode(%{id: "mock", object: "chat.completion",
                  choices: [%{index: 0, finish_reason: "stop",
                              message: %{role: "assistant", content: content}}]})
}

fun count(t, key) { ets_update_counter(t, key, 1, 0) }

fun faults_on(t) { ets_get(t, "faults") != 'false' }

# "EP <id> TURN <t>: ..." -> {id, turn, last word}
fun parse(prompt) {
    w = string_split(prompt, " ")
    if (length(w) < 4 || Std.nth(w, 0) != "EP") { nil }
    else { {to_int(Std.nth(w, 1)), to_int(Std.nth(w, 3)), Std.last(w)} }
}

fun latency_ms(ep, turn) { 50 + ((ep * 31 + turn) * 7919) % 101 }

fun chat(t, conn, body) {
    req = json_decode(body)
    msgs = if (req == nil) { nil } else { map_get(req, 'messages') }
    p = if (msgs == nil || length(msgs) == 0) { nil } else { parse(map_get(Std.last(msgs), 'content')) }
    if (p == nil) {
        count(t, "t:bad")
        http_respond(conn, 400, json_headers(), "{\"error\":{\"message\":\"no EP tag\"}}")
    } else {
        ep = elem(p, 0)
        turn = elem(p, 1)
        calls = count(t, f"c:{ep}")
        if (calls == 1) { count(t, "seen") }
        if (calls == 3) { count(t, "extra") }
        count(t, "req")
        n = count(t, f"a:{ep}:{turn}")
        f = faults_on(t)
        cond {
            f && ep % 20 == 3 && n == 1 -> {
                count(t, "t:429")
                http_respond(conn, 429, json_headers() ++ "Retry-After: 1\r\n",
                             "{\"error\":{\"message\":\"rate limited\",\"type\":\"rate_limit\"}}")
            }
            f && ep % 50 == 11 && n == 1 -> {
                count(t, "t:500")
                http_respond(conn, 500, json_headers(), "{\"error\":{\"message\":\"server error\"}}")
            }
            f && ep % 100 == 7 && turn == 0 -> {
                count(t, "t:hang")
                sleep(3600000)
            }
            _ -> {
                sleep(latency_ms(ep, turn))
                count(t, "t:200")
                reply = if (turn == 0) { f"TOOL {ep}" } else { "ANSWER " ++ elem(p, 2) }
                http_respond(conn, 200, json_headers(), chat_reply(reply))
            }
        }
    }
}

fun get_count(t, key) {
    v = ets_get(t, key)
    if (v == nil) { 0 } else { v }
}

fun stats(t, conn) {
    totals = %{}
    totals = map_put(totals, "200", get_count(t, "t:200"))
    totals = map_put(totals, "429", get_count(t, "t:429"))
    totals = map_put(totals, "500", get_count(t, "t:500"))
    totals = map_put(totals, "hang", get_count(t, "t:hang"))
    http_respond(conn, 200, json_headers(),
                 json_encode(%{totals: totals, requests: get_count(t, "req"),
                               episodes_seen: get_count(t, "seen"),
                               episodes_with_extra_calls: get_count(t, "extra")}))
}

fun reset(t, conn, body) {
    Std.each(ets_list(t), fn(kv) { ets_delete(t, elem(kv, 0)) })
    req = json_decode(body)
    on = if (req == nil) { 'true' } else { map_get(req, 'faults') }
    ets_put(t, "faults", if (on == 'false') { 'false' } else { 'true' })
    http_respond(conn, 200, json_headers(), "{\"ok\":true}")
}

fun handle(t, conn, method, path, body) {
    cond {
        method == 'POST' && path == "/v1/chat/completions" -> chat(t, conn, body)
        method == 'GET' && path == "/stats" -> stats(t, conn)
        method == 'POST' && path == "/reset" -> reset(t, conn, body)
        _ -> http_respond(conn, 404, json_headers(), "{\"error\":{\"message\":\"not found\"}}")
    }
}

fun loop(t) {
    receive {
        {'http_request', conn, method, path, _h, body} ->   # method is an atom: 'GET', 'POST'
            spawn(handle(t, conn, method, path, body))   # one process per request
            loop(t)
        _ -> loop(t)
    }
}

fun main() {
    port = env_int("PORT", 18600)
    t = ets_new()
    ets_put(t, "faults", 'true')
    http_listen(port)
    eprint(f"mock_server.sw: listening on 127.0.0.1:{port}")
    loop(t)
}
