module Test_stream_parallel

# Regression: subagent-mode http_post_stream (the parallel-subagents shape:
# each agent streams its tokens to a parent as {'stream_chunk', name, text})
# must not pin a scheduler thread per stream. It runs on the offload pool;
# 6 streams against an in-VM server that answers after 300ms finish together
# instead of one scheduler-at-a-time.

fun sse() {
    "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n" ++
    "data: [DONE]\n\n"
}

fun server() { http_listen(9155) ; serve() }
fun serve() {
    receive {
        {'http_request', conn, _m, _p, _h, _b} ->
            spawn(fun() { sleep(300) ; http_respond(conn, 200, "Content-Type: text/event-stream\r\n", sse()) })
            serve()
        _other -> serve()
    }
}

fun wait_up(attempts) {
    r = http_request("http://127.0.0.1:9155/ping", %{})
    case r { {'error', _} -> if (attempts <= 1) { 'down' } else { sleep(50) ; wait_up(attempts - 1) } ; _ -> 'up' }
}

fun agent(parent, i) {
    r = http_post_stream("http://127.0.0.1:9155/v1/chat/completions", [], "{}", parent, f"a{i}")
    send(parent, {'agent_done', i, elem(r, 0)})
}

fun start(i, n, me) { if (i == n) { 'ok' } else { spawn(agent(me, i)) ; start(i + 1, n, me) } }

fun collect(done, chunks, n) {
    if (done == n) { {done, chunks} }
    else {
        receive {
            {'agent_done', _i, 'ok'} -> collect(done + 1, chunks, n)
            {'agent_done', _i, _bad} -> collect(done + 1, chunks - 1000, n)
            {'stream_chunk', _name, _text} -> collect(done, chunks + 1, n)
            _other -> collect(done, chunks, n)
            after 10000 -> {done, chunks}
        }
    }
}

fun main() {
    spawn(server())
    if (wait_up(100) != 'up') { print("FAIL server_up") ; sys_exit(1) }
    t0 = timestamp()
    start(0, 6, self())
    res = collect(0, 0, 6)
    elapsed = timestamp() - t0
    fails = 0
    if (elem(res, 0) == 6 && elem(res, 1) >= 6) { print("PASS all_streamed") }
    else { print(f"FAIL all_streamed: {res}") ; fails = 1 }
    if (elapsed < 1500) { print("PASS concurrent") }
    else { print(f"FAIL concurrent: 6 x 300ms streams took {elapsed}ms") ; fails = fails + 1 }
    if (fails == 0) { print("OK stream_parallel 2/2") ; sys_exit(0) } else { sys_exit(1) }
}
