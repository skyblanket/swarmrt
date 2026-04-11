module LiveChat

# SwarmRT Live Chat — streaming AI agent interface
#
# Per-tab process holds conversation history.
# Uses llm_stream for incremental token delivery.
#
# Run:
#   bin/swc build studio/live_chat.sw -o /tmp/live_chat --emit-c
#   /tmp/live_chat
#   → http://localhost:4002

import Live

fun main() {
    http_listen(4002)
    print("[chat] listening on http://localhost:4002")
    http_handler()
}

fun http_handler() {
    receive {
        {'http_request', conn, 'GET', "/", _h, _b} ->
            html = Live.render_page("Chat", chat_render([], ""))
            http_respond(conn, 200, "Content-Type: text/html\r\n", html)
            http_handler()

        {'http_request', conn, 'GET', "/favicon.ico", _h, _b} ->
            http_respond(conn, 204, "", "")
            http_handler()

        {'ws_connect', conn, "/live/ws"} ->
            pid = spawn(chat_view(conn, []))
            ws_set_handler(conn, pid)
            http_handler()

        _other ->
            http_handler()
    }
}

# Per-connection chat view process
fun chat_view(ws, history) {
    html = chat_render(history, "")
    ws_send(ws, json_encode(%{type: "html", body: html}))
    receive {
        {'ws_message', _conn, raw} ->
            msg = json_decode(raw)
            event = map_get(msg, 'event')
            handle_chat_event(ws, history, event, msg)

        {'ws_close', _conn} ->
            print("[chat] client disconnected")
    }
}

fun handle_chat_event(ws, history, event, msg) {
    if (event == "send_message") {
        handle_send(ws, history, msg)
    } else {
        chat_view(ws, history)
    }
}

fun handle_send(ws, history, msg) {
    value = map_get(msg, 'value')
    user_input = extract_message(value)
    if (string_length(user_input) > 0) {
        new_history = list_append(history, {'user', user_input})
        stream_pid = llm_stream(user_input, %{})
        chat_stream(ws, new_history, stream_pid, "")
    } else {
        chat_view(ws, history)
    }
}

fun extract_message(value) {
    if (value == nil) {
        ""
    } else {
        v = map_get(value, 'message')
        if (v == nil) { "" } else { v }
    }
}

# Streaming receive loop — accumulates tokens, re-renders on each chunk
fun chat_stream(ws, history, stream_pid, partial) {
    receive {
        {'llm_token', token} ->
            new_partial = partial ++ token
            html = chat_render(history, new_partial)
            ws_send(ws, json_encode(%{type: "html", body: html}))
            chat_stream(ws, history, stream_pid, new_partial)

        {'llm_done', full_text} ->
            final_history = list_append(history, {'assistant', full_text})
            chat_view(ws, final_history)

        {'ws_message', _conn, _raw} ->
            chat_stream(ws, history, stream_pid, partial)

        {'ws_close', _conn} ->
            print("[chat] client disconnected during stream")
    }
}

# === Render ===

fun chat_render(history, partial) {
    style = "<style>" ++
        ".chat-container{display:flex;flex-direction:column;height:100vh;max-width:800px;margin:0 auto}" ++
        ".chat-header{padding:1rem 1.5rem;border-bottom:1px solid #222}" ++
        ".chat-header h1{font-size:1.2rem;font-weight:600}" ++
        ".chat-messages{flex:1;overflow-y:auto;padding:1rem 1.5rem}" ++
        ".msg{margin-bottom:1rem;max-width:85%}" ++
        ".msg-user{margin-left:auto;background:#1a3a5c;border-radius:12px 12px 4px 12px;padding:0.75rem 1rem}" ++
        ".msg-assistant{background:#1a1a1a;border:1px solid #222;border-radius:12px 12px 12px 4px;padding:0.75rem 1rem}" ++
        ".msg-role{font-size:0.7rem;color:#666;text-transform:uppercase;margin-bottom:0.25rem;letter-spacing:0.05em}" ++
        ".msg-content{white-space:pre-wrap;line-height:1.5}" ++
        ".chat-input{padding:1rem 1.5rem;border-top:1px solid #222;display:flex;gap:0.75rem}" ++
        ".chat-input input{flex:1;font-size:0.95rem}" ++
        ".chat-input button{padding:0.5rem 1.5rem;font-size:0.95rem;background:#2563eb;border-color:#2563eb;color:#fff}" ++
        ".chat-input button:hover{background:#1d4ed8}" ++
        ".streaming{opacity:0.7}" ++
        ".cursor{display:inline-block;width:2px;height:1em;background:#60a5fa;animation:blink 1s step-end infinite;vertical-align:text-bottom}" ++
        "@keyframes blink{50%{opacity:0}}" ++
        "</style>"

    messages_html = render_messages(history, "")

    streaming = if (string_length(partial) > 0) {
        "<div class=\"msg msg-assistant streaming\">" ++
        "<div class=\"msg-role\">assistant</div>" ++
        "<div class=\"msg-content\">" ++ Live.html_escape(partial) ++ "<span class=\"cursor\"></span></div></div>"
    } else { "" }

    input_form = "<form class=\"chat-input\" sw-submit=\"send_message\">" ++
        "<input type=\"text\" name=\"message\" placeholder=\"Type a message...\" autocomplete=\"off\" autofocus />" ++
        "<button type=\"submit\">Send</button></form>"

    style ++
    "<div class=\"chat-container\">" ++
    "<div class=\"chat-header\"><h1>SwarmRT Chat</h1></div>" ++
    "<div class=\"chat-messages\" id=\"messages\">" ++ messages_html ++ streaming ++ "</div>" ++
    input_form ++
    "</div>"
}

fun render_messages(history, acc) {
    if (length(history) == 0) {
        if (string_length(acc) == 0) {
            "<div style=\"text-align:center;color:#444;padding:4rem 0\">" ++
            "<p style=\"font-size:1.1rem\">No messages yet</p>" ++
            "<p style=\"font-size:0.85rem;margin-top:0.5rem\">Send a message to start chatting</p></div>"
        } else {
            acc
        }
    } else {
        render_msg_row(history, acc)
    }
}

fun render_msg_row(history, acc) {
    entry = hd(history)
    role = elem(entry, 0)
    content = elem(entry, 1)
    role_str = to_string(role)
    cls = if (role_str == "user") { "msg-user" } else { "msg-assistant" }
    row = "<div class=\"msg " ++ cls ++ "\">" ++
        "<div class=\"msg-role\">" ++ role_str ++ "</div>" ++
        "<div class=\"msg-content\">" ++ Live.html_escape(to_string(content)) ++ "</div></div>"
    render_messages(tl(history), acc ++ row)
}
