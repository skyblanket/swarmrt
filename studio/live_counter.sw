module LiveCounter

# SwarmRT LiveView Counter Demo
#
# Each browser tab = one SwarmRT process holding state via recursive tail calls.
# Server pushes full HTML over WebSocket, client morphdom diffs the DOM.
#
# Run:
#   bin/swc build studio/live_counter.sw -o /tmp/live_counter --emit-c
#   /tmp/live_counter
#   → http://localhost:4000

import Live

fun main() {
    http_listen(4000)
    print("[live] listening on http://localhost:4000")
    http_handler()
}

# Main HTTP handler loop — receives HTTP requests and WS connections
fun http_handler() {
    receive {
        {'http_request', conn, 'GET', "/", _h, _b} ->
            html = Live.render_page("Counter", counter_render(0))
            http_respond(conn, 200, "Content-Type: text/html\r\n", html)
            http_handler()

        {'http_request', conn, 'GET', "/favicon.ico", _h, _b} ->
            http_respond(conn, 204, "", "")
            http_handler()

        {'ws_connect', conn, "/live/ws"} ->
            pid = spawn(counter_view(conn, 0))
            ws_set_handler(conn, pid)
            http_handler()

        _other ->
            http_handler()
    }
}

# Per-connection view process — owns one WebSocket, holds one counter
fun counter_view(ws, count) {
    html = counter_render(count)
    ws_send(ws, json_encode(%{type: "html", body: html}))
    receive {
        {'ws_message', _conn, raw} ->
            msg = json_decode(raw)
            event = map_get(msg, 'event')
            new_count = counter_event(event, count)
            counter_view(ws, new_count)

        {'ws_close', _conn} ->
            print("[live] client disconnected")
    }
}

# Event handler — returns new count
fun counter_event(event, count) {
    if (event == "inc") {
        count + 1
    } elsif (event == "dec") {
        count - 1
    } elsif (event == "reset") {
        0
    } else {
        count
    }
}

# Render counter HTML (sent over WebSocket as body of live-root div)
fun counter_render(count) {
    "<div style=\"text-align:center;padding:4rem\">" ++
    "<h1 style=\"font-size:2rem;margin-bottom:0.5rem\">SwarmRT LiveView</h1>" ++
    "<p style=\"color:#666;margin-bottom:3rem\">each tab = one process</p>" ++
    "<div style=\"font-size:6rem;font-weight:200;margin:2rem;font-variant-numeric:tabular-nums\">" ++
        to_string(count) ++
    "</div>" ++
    "<div style=\"display:flex;gap:1rem;justify-content:center\">" ++
    "<button sw-click=\"dec\" style=\"font-size:1.5rem;padding:0.75rem 2rem\">-</button>" ++
    "<button sw-click=\"reset\" style=\"font-size:1.5rem;padding:0.75rem 2rem\">Reset</button>" ++
    "<button sw-click=\"inc\" style=\"font-size:1.5rem;padding:0.75rem 2rem\">+</button>" ++
    "</div></div>"
}
