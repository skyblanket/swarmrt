module Observer

# SwarmRT Agent Observer — LiveView dashboard
#
# Shows: process list, ETS tables, registered names
# Auto-refreshes every 2s via tail-call recursion.
#
# Run:
#   bin/swc build studio/observer.sw -o /tmp/observer --emit-c
#   /tmp/observer
#   → http://localhost:4001/observer

import Live

fun main() {
    http_listen(4001)
    print("[observer] listening on http://localhost:4001/observer")
    http_handler()
}

fun http_handler() {
    receive {
        {'http_request', conn, 'GET', "/observer", _h, _b} ->
            html = Live.render_page("Observer", render_dashboard())
            http_respond(conn, 200, "Content-Type: text/html\r\n", html)
            http_handler()

        {'http_request', conn, 'GET', "/", _h, _b} ->
            http_respond(conn, 302, "Location: /observer\r\n", "")
            http_handler()

        {'http_request', conn, 'GET', "/favicon.ico", _h, _b} ->
            http_respond(conn, 204, "", "")
            http_handler()

        {'ws_connect', conn, "/live/ws"} ->
            pid = spawn(observer_view(conn))
            ws_set_handler(conn, pid)
            http_handler()

        _other ->
            http_handler()
    }
}

# Per-tab observer view — auto-refreshes
fun observer_view(ws) {
    html = render_dashboard()
    ws_send(ws, json_encode(%{type: "html", body: html}))
    sleep(2000)
    observer_view(ws)
}

# === Dashboard Render ===

fun render_dashboard() {
    style = "<style>" ++
        "table{width:100%;border-collapse:collapse;font-size:0.85rem}" ++
        "th,td{padding:6px 10px;text-align:left;border-bottom:1px solid #222}" ++
        "th{color:#888;font-weight:500;text-transform:uppercase;font-size:0.75rem;letter-spacing:0.05em}" ++
        "tr:hover{background:#1a1a1a}" ++
        ".section{margin-bottom:2rem}" ++
        ".section-title{font-size:1.1rem;font-weight:600;margin-bottom:0.75rem;color:#ccc}" ++
        ".stat{display:inline-block;padding:0.5rem 1.5rem;background:#111;border:1px solid #222;border-radius:8px;margin-right:0.75rem;margin-bottom:0.75rem}" ++
        ".stat-val{font-size:1.5rem;font-weight:300;font-variant-numeric:tabular-nums}" ++
        ".stat-label{font-size:0.7rem;color:#666;text-transform:uppercase;letter-spacing:0.05em}" ++
        ".status-running{color:#4ade80}" ++
        ".status-waiting{color:#facc15}" ++
        ".status-runnable{color:#60a5fa}" ++
        ".pid-col{font-family:monospace;color:#888}" ++
        "</style>"

    # Gather data
    procs = process_list()
    regs = registered()
    tables = ets_list()

    proc_count = to_string(length(procs))
    reg_count = to_string(length(regs))
    ets_count_str = to_string(length(tables))

    # Stats bar
    stats = "<div style=\"padding:1.5rem 2rem\">" ++
        "<div class=\"stat\"><div class=\"stat-val\">" ++ proc_count ++ "</div><div class=\"stat-label\">Processes</div></div>" ++
        "<div class=\"stat\"><div class=\"stat-val\">" ++ reg_count ++ "</div><div class=\"stat-label\">Registered</div></div>" ++
        "<div class=\"stat\"><div class=\"stat-val\">" ++ ets_count_str ++ "</div><div class=\"stat-label\">ETS Tables</div></div>" ++
        "</div>"

    # Process table
    proc_rows = render_proc_rows(procs, "")
    proc_table = "<div class=\"section\" style=\"padding:0 2rem\">" ++
        "<div class=\"section-title\">Processes</div>" ++
        "<table><thead><tr>" ++
        "<th>PID</th><th>Name</th><th>Status</th><th>Reductions</th><th>Messages</th><th>Heap</th>" ++
        "</tr></thead><tbody>" ++ proc_rows ++ "</tbody></table></div>"

    # Registered names table
    reg_rows = render_reg_rows(regs, "")
    reg_table = "<div class=\"section\" style=\"padding:0 2rem\">" ++
        "<div class=\"section-title\">Registered Names</div>" ++
        "<table><thead><tr>" ++
        "<th>Name</th><th>PID</th>" ++
        "</tr></thead><tbody>" ++ reg_rows ++ "</tbody></table></div>"

    # ETS table
    ets_rows = render_ets_rows(tables, "")
    ets_table = "<div class=\"section\" style=\"padding:0 2rem\">" ++
        "<div class=\"section-title\">ETS Tables</div>" ++
        "<table><thead><tr>" ++
        "<th>Table ID</th><th>Entries</th>" ++
        "</tr></thead><tbody>" ++ ets_rows ++ "</tbody></table></div>"

    header = "<div style=\"padding:1.5rem 2rem;border-bottom:1px solid #222\">" ++
        "<h1 style=\"font-size:1.3rem;font-weight:600\">SwarmRT Observer</h1>" ++
        "<p style=\"color:#666;font-size:0.8rem;margin-top:0.25rem\">auto-refresh 2s</p></div>"

    style ++ header ++ stats ++ proc_table ++ reg_table ++ ets_table
}

# Recursive list renderer for processes
fun render_proc_rows(procs, acc) {
    if (length(procs) == 0) {
        acc
    } else {
        p = hd(procs)
        info = process_info(p)
        pid_str = to_string(map_get(info, 'pid'))
        pname = map_get(info, 'name')
        name_str = if (pname == nil) { "-" } else { to_string(pname) }
        status = to_string(map_get(info, 'status'))
        reds = to_string(map_get(info, 'reductions'))
        msgs = to_string(map_get(info, 'messages'))
        heap = to_string(map_get(info, 'heap_used'))
        status_class = "status-" ++ status
        row = "<tr>" ++
            "<td class=\"pid-col\">" ++ pid_str ++ "</td>" ++
            "<td>" ++ name_str ++ "</td>" ++
            "<td class=\"" ++ status_class ++ "\">" ++ status ++ "</td>" ++
            "<td>" ++ reds ++ "</td>" ++
            "<td>" ++ msgs ++ "</td>" ++
            "<td>" ++ heap ++ "</td></tr>"
        render_proc_rows(tl(procs), acc ++ row)
    }
}

# Recursive list renderer for registered names
fun render_reg_rows(regs, acc) {
    if (length(regs) == 0) {
        acc
    } else {
        entry = hd(regs)
        name = to_string(elem(entry, 0))
        pid_str = to_string(elem(entry, 1))
        row = "<tr><td>" ++ name ++ "</td><td class=\"pid-col\">" ++ pid_str ++ "</td></tr>"
        render_reg_rows(tl(regs), acc ++ row)
    }
}

# Recursive list renderer for ETS tables
fun render_ets_rows(tables, acc) {
    if (length(tables) == 0) {
        acc
    } else {
        tid = hd(tables)
        count = to_string(ets_count(tid))
        row = "<tr><td class=\"pid-col\">" ++ to_string(tid) ++ "</td><td>" ++ count ++ "</td></tr>"
        render_ets_rows(tl(tables), acc ++ row)
    }
}
