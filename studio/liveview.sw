module LiveView

# ============================================================
# LiveView — server-driven reactive UI for swarmrt
# ============================================================
#
# The server holds the UI state; the browser is a thin renderer.
# Each connected tab is one sw process. After the first paint, only
# the values that *changed* cross the WebSocket — never the page.
#
# How the diff works — the static/dynamic split:
#   A `render` returns a structure, NOT a flat string:
#       %{ s: [static html segments],  d: [dynamic values] }
#   The static segments are redundancy — sent once, at mount. The
#   dynamics are the signal — re-evaluated every render and diffed;
#   only the changed ones are pushed. A counter increment costs the
#   index + the new number, not a re-rendered page.
#
# Authoring a view — a map of callbacks (plain named functions):
#   fun mount(params)               -> initial assigns map
#   fun render(assigns)             -> LiveView.h(template, [dynamics])
#   fun handle_event(name,val,asg)  -> new assigns
#   fun handle_info(msg,asg)        -> new assigns        (optional)
#   LiveView.start(4000, %{ mount: mount, render: render,
#                           handle_event: handle_event, title: "App" })
#
# Templates use {{}} as a positional hole. Splitting the template on
# {{}} *is* the static/dynamic seam — and {{}} never collides with
# CSS/JS, which only ever use single braces.
#
# Wire protocol (the embedded client in swarmrt_liveview_js.h):
#   server -> client   {t:"m", s:[statics], d:[dynamic strings]}   mount
#   server -> client   {t:"d", c:{"idx":"newval", ...}}            diff
#   client -> server   {t:"e", n:"event", v:{...}}                 event

export [start, h, raw, assign, esc, page]

# ------------------------------------------------------------
# Templates
# ------------------------------------------------------------
# h(template, dynamics) -> a rendered struct %{s, d}. The static
# segments come straight out of string_split — parse once, here.
fun h(tpl, dynamics) {
    %{ s: string_split(tpl, "{{}}"), d: dynamics }
}

# Mark a string as raw, un-escaped HTML — for composed sub-renders
# (e.g. a list of rows the view built itself). Plain dynamics are
# HTML-escaped; raw ones are not.
fun raw(html) {
    %{ raw: to_string(html) }
}

# Set one key in the assigns map.
fun assign(assigns, key, val) {
    map_put(assigns, key, val)
}

# ------------------------------------------------------------
# Stitching — rendered struct -> HTML string
# ------------------------------------------------------------
# Resolve one dynamic value to its HTML string:
#   nested rendered struct (%{s,d}) -> recurse
#   raw marker (%{raw})             -> verbatim
#   anything else                   -> escaped text
fun stitch_dyn(d) {
    if (d == nil) { "" }
    else { if (is_map(d) == 'false') { esc(to_string(d)) }
    else {
        nested = map_get(d, 's')
        if (nested != nil) {
            stitch(d)
        } else {
            rawv = map_get(d, 'raw')
            if (rawv != nil) { to_string(rawv) }
            else { esc(to_string(d)) }
        }
    }}
}

# Interleave statics with dynamics: s0 ++ d0 ++ s1 ++ d1 ++ ... ++ sN.
fun stitch(rendered) {
    stitch_loop(map_get(rendered, 's'), map_get(rendered, 'd'), "")
}

fun stitch_loop(statics, dyns, acc) {
    if (statics == nil || length(statics) == 0) { acc }
    else {
        acc2 = acc ++ to_string(hd(statics))
        if (dyns == nil || length(dyns) == 0) {
            stitch_loop(tl(statics), [], acc2)
        } else {
            stitch_loop(tl(statics), tl(dyns), acc2 ++ stitch_dyn(hd(dyns)))
        }
    }
}

# Resolve a list of dynamics to a list of HTML strings — this is what
# the diff compares and what the wire carries. Nested structs and raw
# markers are flattened to strings here (v1; finer per-component
# diffing is a Phase 2 refinement).
fun dyn_strings(dyns, acc) {
    if (dyns == nil || length(dyns) == 0) { acc }
    else { dyn_strings(tl(dyns), list_append(acc, stitch_dyn(hd(dyns)))) }
}

# ------------------------------------------------------------
# Diff — old dynamic strings vs new -> map of changed indices
# ------------------------------------------------------------
fun diff(old_ds, new_ds, i, acc) {
    if (new_ds == nil || length(new_ds) == 0) { acc }
    else {
        nv = hd(new_ds)
        ov = if (old_ds == nil || length(old_ds) == 0) { nil } else { hd(old_ds) }
        acc2 = if (nv == ov) { acc } else { map_put(acc, to_string(i), nv) }
        old_rest = if (old_ds == nil || length(old_ds) == 0) { [] } else { tl(old_ds) }
        diff(old_rest, tl(new_ds), i + 1, acc2)
    }
}

fun list_eq(a, b) {
    if (length(a) != length(b)) { 'false' }
    else { if (length(a) == 0) { 'true' }
    else { if (hd(a) == hd(b)) { list_eq(tl(a), tl(b)) }
    else { 'false' }}}
}

# ------------------------------------------------------------
# HTML escaping
# ------------------------------------------------------------
fun esc(s) {
    e1 = string_replace(to_string(s), "&", "&amp;")
    e2 = string_replace(e1, "<", "&lt;")
    e3 = string_replace(e2, ">", "&gt;")
    string_replace(e3, "\"", "&quot;")
}

# ------------------------------------------------------------
# Page shell — the dead-render HTML wrapper + embedded client
# ------------------------------------------------------------
fun page(title, body) {
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">" ++
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">" ++
    "<title>" ++ esc(title) ++ "</title>" ++
    "<style>*{margin:0;padding:0;box-sizing:border-box}" ++
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;" ++
    "background:#0a0a0a;color:#e6e6e6}" ++
    "button{cursor:pointer;border:1px solid #333;background:#1a1a1a;color:#e6e6e6;" ++
    "border-radius:8px;transition:all .15s}button:hover{background:#2a2a2a;border-color:#555}" ++
    "input,textarea,select{background:#1a1a1a;color:#e6e6e6;border:1px solid #333;" ++
    "border-radius:6px;padding:8px 12px;outline:none}</style>" ++
    "</head><body><div id=\"live-root\">" ++ body ++ "</div>" ++
    "<script>" ++ live_js() ++ "</script></body></html>"
}

fun lv_title(view) {
    t = map_get(view, 'title')
    if (t == nil) { "SwarmRT LiveView" } else { to_string(t) }
}

fun render_view(view, assigns) {
    render_fn = map_get(view, 'render')
    render_fn(assigns)
}

# ------------------------------------------------------------
# start — listen, then serve the HTTP/WS loop
# ------------------------------------------------------------
fun start(port, view) {
    http_listen(port)
    print("[liveview] listening on http://localhost:" ++ to_string(port))
    serve(view)
}

# The accept loop. A plain GET gets the server-rendered "dead" page
# (instant first paint, works with JS off / for crawlers); the page's
# embedded client then upgrades to /live/ws and the session takes over.
fun serve(view) {
    receive {
        {'http_request', conn, 'GET', "/favicon.ico", _h, _b} ->
            http_respond(conn, 204, "", "")
            serve(view)

        {'http_request', conn, 'GET', _path, _h, _b} ->
            mount_fn = map_get(view, 'mount')
            assigns = mount_fn(map_new())
            body = stitch(render_view(view, assigns))
            http_respond(conn, 200, "Content-Type: text/html\r\n",
                         page(lv_title(view), body))
            serve(view)

        {'ws_connect', conn, "/live/ws"} ->
            pid = spawn(session(conn, view))
            ws_set_handler(conn, pid)
            serve(view)

        _other ->
            serve(view)
    }
}

# ------------------------------------------------------------
# session — one process per connected tab
# ------------------------------------------------------------
# mount -> render -> send the full {statics, dynamics}; then loop,
# pushing only diffs.
fun session(conn, view) {
    mount_fn = map_get(view, 'mount')
    assigns = mount_fn(map_new())
    rendered = render_view(view, assigns)
    statics = map_get(rendered, 's')
    dstrings = dyn_strings(map_get(rendered, 'd'), [])
    ws_send(conn, json_encode(%{t: "m", s: statics, d: dstrings}))
    session_loop(conn, view, assigns, statics, dstrings)
}

fun session_loop(conn, view, assigns, last_s, last_d) {
    receive {
        {'ws_message', _c, raw_msg} ->
            msg = json_decode(raw_msg)
            if (msg == nil) {
                session_loop(conn, view, assigns, last_s, last_d)
            } else {
                he = map_get(view, 'handle_event')
                new_assigns = if (he == nil) { assigns }
                              else { he(map_get(msg, 'n'), map_get(msg, 'v'), assigns) }
                push_update(conn, view, new_assigns, last_s, last_d)
            }

        {'ws_close', _c} ->
            'ok'

        other ->
            hi = map_get(view, 'handle_info')
            if (hi == nil) {
                session_loop(conn, view, assigns, last_s, last_d)
            } else {
                push_update(conn, view, hi(other, assigns), last_s, last_d)
            }
    }
}

# Re-render against new assigns. If the static structure is unchanged
# (the common case), push only the changed dynamics. If the structure
# itself changed (a conditional flipped, a list grew), fall back to a
# fresh mount — correct, just less frugal; Phase 2 makes structural
# change incremental too.
fun push_update(conn, view, assigns, last_s, last_d) {
    rendered = render_view(view, assigns)
    statics = map_get(rendered, 's')
    dstrings = dyn_strings(map_get(rendered, 'd'), [])
    if (list_eq(statics, last_s) == 'false') {
        ws_send(conn, json_encode(%{t: "m", s: statics, d: dstrings}))
    } else {
        changes = diff(last_d, dstrings, 0, map_new())
        if (length(map_keys(changes)) > 0) {
            ws_send(conn, json_encode(%{t: "d", c: changes}))
        } else {
            'noop'
        }
    }
    session_loop(conn, view, assigns, statics, dstrings)
}
