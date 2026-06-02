module Test_voice_ws_loopback

# End-to-end WebSocket data-path proof with NO live credentials:
#   - a swarmrt WS *server* (http_listen) echoes whatever text frame it
#     receives back to the sender,
#   - a swarmrt WS *client* (wsc_connect, plaintext ws://) connects, sends
#     a base64 mu-law frame, and reads the echo,
#   - we assert the echoed base64 payload is byte-identical to what we
#     sent — proving the audio bytes survive server-in -> client-out with
#     zero mutation (the g711-passthrough guarantee, minus the TLS leg).
#
# This exercises the real frame loop in src/swarmrt_http.c (masked client
# frames in, server frames out) and the wsc_* client builtins together.
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
    http_listen(9099)
    echo_loop()
}

fun echo_loop() {
    receive {
        {'ws_message', conn, text} ->
            ws_send(conn, text)     # echo verbatim
            echo_loop()
        {'ws_binary', conn, b64} ->
            # binary frames are delivered base64-encoded; echo as text
            ws_send(conn, b64)
            echo_loop()
        {'ws_close', _conn} ->
            echo_loop()
    }
}

fun main() {
    spawn(fun() { echo_server() })
    sleep(200)   # let the listener bind

    h = wsc_connect("ws://127.0.0.1:9099/")
    fails = 0
    case h {
        'nil' ->
            print("FAIL ws_loopback_connect: wsc_connect returned nil")
            fails = 1
        _ ->
            # a representative base64 mu-law frame (8 codes, no NULs)
            payload = "/4AAVaoRwzw="
            wsc_send(h, payload)
            echo = wsc_recv(h, 3000)
            fails = fails + assert_eq("ws_loopback_passthrough", echo, payload)
            wsc_close(h)
    }

    if (fails == 0) { print("OK test_voice_ws_loopback 1/1") sys_exit(0) }
    else { print(f"FAIL test_voice_ws_loopback {fails} failed") sys_exit(1) }
}
