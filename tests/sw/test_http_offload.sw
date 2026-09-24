module Test_http_offload

# Regression: blocking HTTP client builtins must NOT pin their scheduler
# thread. Each http_get spawns curl and waits for it; that wait used to run
# on the scheduler OS thread, so with N schedulers only N requests could be
# in flight (16 parallel 1s requests took 4s on 4 schedulers) and a program
# that served AND called itself over HTTP deadlocked on one scheduler. The
# builtins now run on the runtime's offload pool while the caller parks.
#
# An in-VM server answers every request after a 300ms delay (each reply on
# its own process). 8 clients call it concurrently. Serialized that is
# >= 2.4s; offloaded it is ~0.3s. We allow a generous 1.5s to stay robust
# on a loaded CI host, and the test must pass even with SW_SCHEDULERS=1.

fun slow_server() {
    http_listen(9151)
    slow_loop()
}

fun slow_loop() {
    receive {
        {'http_request', conn, _method, _path, _headers, _body} ->
            spawn(fun() { sleep(300) ; http_respond(conn, 200, "", "slow-ok") })
            slow_loop()
        _other -> slow_loop()
    }
}

fun wait_up(attempts) {
    r = http_request("http://127.0.0.1:9151/ping", %{})
    case r {
        {'error', _} -> if (attempts <= 1) { 'down' } else { sleep(50) ; wait_up(attempts - 1) }
        _ -> 'up'
    }
}

fun client(parent, i) {
    body = http_get("http://127.0.0.1:9151/x")
    send(parent, {'got', i, body})
}

fun spawn_clients(i, n, me) {
    if (i == n) { 'ok' } else { spawn(client(me, i)) ; spawn_clients(i + 1, n, me) }
}

fun collect(n, ok) {
    if (n == 0) { ok }
    else {
        receive {
            {'got', _i, body} -> collect(n - 1, if (body == "slow-ok") { ok + 1 } else { ok })
            after 10000 -> ok
        }
    }
}

fun main() {
    spawn(slow_server())
    state = wait_up(100)
    if (state != 'up') { print("FAIL server_up") ; sys_exit(1) }
    t0 = timestamp()
    spawn_clients(0, 8, self())
    ok = collect(8, 0)
    elapsed = timestamp() - t0
    fails = 0
    if (ok == 8) { print("PASS all_replies") } else { print(f"FAIL all_replies: {ok}/8") ; fails = 1 }
    if (elapsed < 1500) { print("PASS concurrent") }
    else { print(f"FAIL concurrent: 8 x 300ms requests took {elapsed}ms (serialized?)") ; fails = fails + 1 }
    if (fails == 0) { print("OK http_offload 2/2") ; sys_exit(0) }
    else { print("FAIL http_offload") ; sys_exit(1) }
}
