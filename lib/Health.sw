# Health.sw — liveness/readiness endpoint for operators, pure sw over the
# http_listen/http_respond builtins + the Phase-4 swarm_stats() metrics map.
#
#   import Health
#   Health.start(8080)        # spawn the server, keep working
#   # ...or Health.serve(8080) to run it in the calling process (blocks)
#
#   curl http://localhost:8080/healthz   → 200 "ok"        (liveness)
#   curl http://localhost:8080/readyz    → 200 swarm_stats() as JSON (readiness)
#   curl http://localhost:8080/processes → 200 JSON array, one row per process
#   curl http://localhost:8080/tree      → 200 JSON supervision forest
#   curl http://localhost:8080/top?by=mailbox&n=20 → 200 JSON worst offenders
#
# Machine-readable introspection (JSON is the primary surface; any UI is a
# skin over it). All three read process_list()/process_info() live:
#
#   /processes[?limit=N]  [{pid, parent, name, status, mailbox_len, memory,
#                           messages_sent, messages_recv}, ...]. parent and
#                         name are null when unset. With limit=N only the
#                         first N rows are returned, followed by one summary
#                         row {summary: true, omitted, mailbox_len, memory,
#                         messages_sent, messages_recv} totalling the rest,
#                         so a 90,000-process node doesn't make a 4 MB reply.
#   /tree                 what Swarm.tree() returns: root processes, each
#                         with a "children" array (full process_info maps).
#   /top?by=B&n=N         the N worst processes, ranked server-side, in the
#                         /processes row shape, worst first. B is one of
#                         mailbox (default; pending depth), memory (value-arena
#                         bytes) or rate (messages_sent + messages_recv since
#                         spawn — lifetime volume, no sampling window). N
#                         defaults to 20, capped at 1000. Unknown B → 400.
#
# /readyz additionally carries resident_bytes (process RSS) and, per entry
# of scheduler_stats, utilization — the percentage of scheduler loop
# iterations that did not end in an idle wait (lifetime, best-effort).
#
# /healthz answers 200 the moment the scheduler can run this process — a
# liveness probe. /readyz additionally serializes the live swarm_stats()
# map (process count, spawns/crashes/restarts, drop counters, per-scheduler
# stats), so an operator or readiness probe sees real runtime state.
# Anything else is 404. COMPILED-ONLY in practice: http_listen needs the
# scheduler + IO bridge of a `swc build` binary.
#
# Binds 127.0.0.1 like every http_listen server; set SW_HTTP_BIND=0.0.0.0
# when the probe comes from another host (a Kubernetes kubelet, a load
# balancer).

module Health

import Swarm
import Std

export [start, serve]

# start(port) — run the health server in its own process; returns its pid.
fun start(port) {
    spawn(serve(port))
}

# serve(port) — bind and serve in the CALLING process (never returns while
# the server lives). Panics loudly if the port can't be bound — a health
# endpoint that silently isn't there is worse than a crash.
fun serve(port) {
    case http_listen(port) {
        'ok' -> _loop()
        _    -> panic(f"Health.serve: could not listen on port {port}")
    }
}

fun _loop() {
    receive {
        {'http_request', conn, method, path, headers, body} ->
            _respond(conn, path)
            _loop()
    }
}

fun _respond(conn, path) {
    case path {
        "/healthz" -> http_respond(conn, 200, "Content-Type: text/plain\r\n", "ok\n")
        _          -> _respond_route(conn, string_split(path, "?"))
    }
}

fun _respond_route(conn, parts) {
    qs = if (length(parts) > 1) { hd(tl(parts)) } else { "" }
    case hd(parts) {
        "/readyz"     -> _json(conn, 200, _readyz())
        "/processes"  -> _json(conn, 200, _processes(_int_param(qs, "limit", 0)))
        "/tree"       -> _json(conn, 200, Swarm.tree())
        "/top"        -> _top(conn, _param(qs, "by", "mailbox"), _int_param(qs, "n", 20))
        _             -> http_respond(conn, 404, "Content-Type: text/plain\r\n", "not found\n")
    }
}

fun _json(conn, status, value) {
    http_respond(conn, status, "Content-Type: application/json\r\n",
                 json_encode(value) ++ "\n")
}

# ---- /readyz ----

fun _readyz() {
    st = swarm_stats()
    map_put(st, 'scheduler_stats',
            map(fun(sc) { map_put(sc, 'utilization', _util(sc)) },
                map_get(st, 'scheduler_stats')))
}

fun _util(sc) {
    iters = map_get(sc, 'loop_iters')
    idle = map_get(sc, 'idle_waits')
    if (iters <= 0) { 0 } else { ((iters - idle) * 100) / iters }
}

# ---- /processes, /top ----

fun _row(info) {
    %{pid: map_get(info, 'pid'), parent: map_get(info, 'parent'),
      name: map_get(info, 'name'), status: map_get(info, 'status'),
      mailbox_len: map_get(info, 'mailbox_len'), memory: map_get(info, 'memory'),
      messages_sent: map_get(info, 'messages_sent'),
      messages_recv: map_get(info, 'messages_recv')}
}

fun _rows() {
    map(fun(p) { _row(process_info(p)) }, filter(fun(p) { process_info(p) != nil }, process_list()))
}

fun _processes(limit) {
    rows = _rows()
    if (limit > 0 && length(rows) > limit) {
        rest = Std.drop(rows, limit)
        Std.take(rows, limit) ++ [_summary(rest)]
    } else { rows }
}

fun _summary(rows) {
    %{summary: 'true', omitted: length(rows),
      mailbox_len: _sum(rows, 'mailbox_len'), memory: _sum(rows, 'memory'),
      messages_sent: _sum(rows, 'messages_sent'),
      messages_recv: _sum(rows, 'messages_recv')}
}

fun _sum(rows, key) {
    Std.reduce(rows, 0, fun(acc, r) { acc + map_get(r, key) })
}

fun _top(conn, by, n) {
    key = case by {
        "mailbox" -> 'mailbox_len'
        "memory"  -> 'memory'
        "rate"    -> 'rate'
        _         -> 'bad'
    }
    if (key == 'bad') {
        http_respond(conn, 400, "Content-Type: text/plain\r\n",
                     "bad request: by must be mailbox, memory or rate\n")
    } else {
        limit = if (n < 1) { 20 } else { if (n > 1000) { 1000 } else { n } }
        _json(conn, 200, _worst(_rows(), key, limit, []))
    }
}

fun _score(r, key) {
    if (key == 'rate') { map_get(r, 'messages_sent') + map_get(r, 'messages_recv') }
    else { map_get(r, key) }
}

# _worst — bounded selection: keep the `limit` highest-scoring rows in
# `best` (descending), O(rows * limit) rather than sorting every process.
fun _worst(rows, key, limit, best) {
    if (length(rows) == 0) { best }
    else { _worst(tl(rows), key, limit, _keep(hd(rows), key, limit, best)) }
}

fun _keep(r, key, limit, best) {
    if (length(best) >= limit && _score(r, key) <= _score(_last(best), key)) { best }
    else { Std.take(_insert(r, key, best, []), limit) }
}

fun _insert(r, key, best, acc) {
    if (length(best) == 0) { Std.reverse(acc) ++ [r] }
    else {
        if (_score(r, key) > _score(hd(best), key)) { Std.reverse(acc) ++ [r] ++ best }
        else { _insert(r, key, tl(best), [hd(best) | acc]) }
    }
}

fun _last(lst) {
    if (length(lst) <= 1) { hd(lst) } else { _last(tl(lst)) }
}

# ---- query string ----

fun _param(qs, key, dflt) {
    _find_param(string_split(qs, "&"), key, dflt)
}

fun _find_param(pairs, key, dflt) {
    if (length(pairs) == 0) { dflt }
    else {
        kv = string_split(hd(pairs), "=")
        if (length(kv) == 2 && hd(kv) == key) { hd(tl(kv)) }
        else { _find_param(tl(pairs), key, dflt) }
    }
}

fun _int_param(qs, key, dflt) {
    v = _param(qs, key, "")
    if (v == "") { dflt } else { to_int(v) }
}
