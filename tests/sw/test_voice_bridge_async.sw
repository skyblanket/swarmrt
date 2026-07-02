module Test_voice_bridge_async

# Multiplexing proof — the property examples/voice_agent.sw relies on:
# ONE process can `receive` on MULTIPLE async wsc handlers at once, with
# each handler's reader pushing into the SAME mailbox, and the single
# selective-receive demuxes them by handle. (In the voice agent the two
# sources are a WS-server leg + one wsc leg; here we use two wsc legs,
# which exercises the exact runtime path — multiple readers, one mailbox,
# one `receive` loop — without any orchestration fragility.)
#
# No live credentials; plaintext ws:// against a local echo server.
# Exits non-zero on failure.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun echo_server(port) {
    http_listen(port)
    echo_loop()
}

fun echo_loop() {
    receive {
        {'ws_message', conn, text} -> ws_send(conn, text) ; echo_loop()
        {'ws_binary', conn, b64}   -> ws_send(conn, b64)  ; echo_loop()
        {'ws_close', _conn}        -> echo_loop()
    }
}

# Collect one frame from EACH of two handles, in whatever order they land,
# from a SINGLE receive loop. Returns {data_a, data_b} or 'timeout'.
fun collect(ha, hb, got_a, got_b) {
    if (got_a != 'pending' && got_b != 'pending') {
        {got_a, got_b}
    } else {
        receive {
            {'wsc_message', hh, data} ->
                if (hh == ha) { collect(ha, hb, data, got_b) }
                else {
                    if (hh == hb) { collect(ha, hb, got_a, data) }
                    else { collect(ha, hb, got_a, got_b) }
                }
            after 3000 { 'timeout' }
        }
    }
}

# Retry the connect with backoff: under parallel-suite CPU contention the
# spawned echo server's bind+handshake can race past a fixed sleep. Matches
# the hardening in test_voice_wsc_async (was a brittle sleep+single-connect).
fun connect_retry(url, attempts) {
    h = wsc_connect(url)
    case h {
        'nil' ->
            if (attempts <= 1) { 'nil' }
            else {
                sleep(50)
                connect_retry(url, attempts - 1)
            }
        _ -> h
    }
}

fun main() {
    spawn(fun() { echo_server(9095) })
    sleep(200)

    fails = 0
    ha = connect_retry("ws://127.0.0.1:9095/", 100)
    hb = connect_retry("ws://127.0.0.1:9095/", 100)
    case ha {
        'nil' -> print("FAIL bridge_async_connect_a") ; fails = 1
        _ ->
            case hb {
                'nil' -> print("FAIL bridge_async_connect_b") ; fails = 1
                _ ->
                    fails = fails + assert_eq("bridge_distinct_handles",
                                              ha == hb, 'false')
                    wsc_set_handler(ha, self())
                    wsc_set_handler(hb, self())

                    # Fire down both legs; both echoes must demux back to us.
                    wsc_send(ha, "leg-a")
                    wsc_send(hb, "leg-b")

                    both = collect(ha, hb, 'pending', 'pending')
                    case both {
                        'timeout' ->
                            print("FAIL bridge_async: timed out on one leg")
                            fails = 1
                        {da, db} ->
                            fails = fails + assert_eq("bridge_async_leg_a", da, "leg-a")
                            fails = fails + assert_eq("bridge_async_leg_b", db, "leg-b")
                    }
                    wsc_close(ha)
                    wsc_close(hb)
            }
    }

    if (fails == 0) { print("OK test_voice_bridge_async 3/3") sys_exit(0) }
    else { print(f"FAIL test_voice_bridge_async {fails} failed") sys_exit(1) }
}
