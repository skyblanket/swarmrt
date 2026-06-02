module Test_voice_wsc_async

# Async wsc push-delivery proof — the inverse of test_voice_ws_loopback.
#
# Instead of POLLING the OpenAI-side client socket with wsc_recv (the old
# `after 5` workaround in examples/voice_agent.sw), this test proves the
# new wsc_set_handler(handle, pid) path: a swarmrt WS *client* registers
# its own process as the handler, and the runtime delivers each inbound
# frame to that process's MAILBOX as {'wsc_message', handle, data}, exactly
# mirroring the WS server's {'ws_message', conn, text} convention. The
# client can then `receive` on it — no poll loop, no blocking wsc_recv.
#
# Topology (no live credentials, plaintext ws://):
#   - a swarmrt WS *server* (http_listen) echoes whatever text frame it gets,
#   - main() connects as a WS *client*, sets itself as the wsc handler,
#     fires TWO frames, and asserts it RECEIVES both back as
#     {'wsc_message', h, <same bytes>} tuples in its mailbox,
#   - then closes and asserts it RECEIVES the {'wsc_close', h} notification.
#
# Exits non-zero on failure so the driver rolls it up.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# Echo server: becomes the WS handler for whatever connects, mirrors text.
fun echo_server() {
    http_listen(9097)
    echo_loop()
}

fun echo_loop() {
    receive {
        {'ws_message', conn, text} ->
            ws_send(conn, text)     # echo verbatim
            echo_loop()
        {'ws_binary', conn, b64} ->
            ws_send(conn, b64)
            echo_loop()
        {'ws_close', _conn} ->
            echo_loop()
    }
}

# Block in `receive` for ONE {'wsc_message', h, data} for this handle.
# Returns the data string, or 'timeout' if nothing lands in 3s.
fun recv_frame(h) {
    receive {
        {'wsc_message', hh, data} ->
            if (hh == h) { data } else { recv_frame(h) }
        after 3000 { 'timeout' }
    }
}

# Block for the {'wsc_close', h} notification. 'closed' | 'timeout'.
fun recv_close(h) {
    receive {
        {'wsc_close', hh} ->
            if (hh == h) { 'closed' } else { recv_close(h) }
        {'wsc_message', _hh, _data} -> recv_close(h)   # drain any straggler
        after 3000 { 'timeout' }
    }
}

fun main() {
    spawn(fun() { echo_server() })
    sleep(200)   # let the listener bind

    h = wsc_connect("ws://127.0.0.1:9097/")
    fails = 0
    case h {
        'nil' ->
            print("FAIL wsc_async_connect: wsc_connect returned nil")
            fails = 1
        _ ->
            # Register THIS process as the handler. From here frames arrive
            # as mailbox messages we can `receive` — no wsc_recv poll.
            sh = wsc_set_handler(h, self())
            fails = fails + assert_eq("wsc_set_handler_ok", sh, 'ok')

            # Frame 1: representative base64 mu-law (8 codes, no NULs).
            p1 = "/4AAVaoRwzw="
            wsc_send(h, p1)
            got1 = recv_frame(h)
            fails = fails + assert_eq("wsc_async_frame1", got1, p1)

            # Frame 2: a second, distinct payload — proves the reader keeps
            # delivering, not just a one-shot.
            p2 = "hello-async-wsc"
            wsc_send(h, p2)
            got2 = recv_frame(h)
            fails = fails + assert_eq("wsc_async_frame2", got2, p2)

            # Close → expect the {'wsc_close', h} mailbox notification.
            wsc_close(h)
            cl = recv_close(h)
            fails = fails + assert_eq("wsc_async_close", cl, 'closed')
    }

    if (fails == 0) { print("OK test_voice_wsc_async 4/4") sys_exit(0) }
    else { print(f"FAIL test_voice_wsc_async {fails} failed") sys_exit(1) }
}
