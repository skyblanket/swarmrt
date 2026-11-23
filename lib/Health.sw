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
#   curl 'http://localhost:8080/top?by=mailbox&n=20' → 200 JSON worst offenders
#
# Machine-readable introspection: JSON is the primary surface for agents, CI
# gates and tools; any UI is a skin over it. All of it reads process_list()
# / process_info() / swarm_stats() live, at request time.
#
#   /readyz               swarm_stats(), which carries resident_bytes (node
#                         RSS, -1 if the platform can't say), plus, in each
#                         scheduler_stats entry, utilization: the percentage
#                         of that scheduler's loop iterations since boot that
#                         did not end in an idle wait. Iterations, not time:
#                         a rough busy signal, not a CPU-time share.
#   /processes[?limit=N]  [{pid, parent, name, status, mailbox_len, memory,
#                           resident_bytes, messages_sent, messages_recv}, ...]
#                         in process-table order; parent and name are null
#                         when unset. With limit=N (N >= 1) the first N rows
#                         are followed by ONE summary row {summary: true,
#                         omitted, mailbox_len, memory, resident_bytes,
#                         messages_sent, messages_recv} totalling the rest, so
#                         a 90,000-process node needn't send a 4 MB reply.
#   /tree                 Swarm.tree(): root processes, each a process_info
#                         map with a "children" array. Quadratic in the
#                         process count; for very large nodes use /processes
#                         (each row has parent) and group client-side.
#   /top?by=B&n=N         the N worst processes, worst first, as /processes
#                         rows. B: mailbox (default; pending depth), memory
#                         (value-arena bytes), resident (resident_bytes) or
#                         rate (messages_sent + messages_recv since spawn:
#                         lifetime volume, there is no sampling window). N
#                         defaults to 20, at most 1000.
#
# A malformed query (limit or n not a whole number >= 1, an unknown `by`)
# is a 400 with a one-line reason, never a silently-defaulted 200.
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
    parts = string_split(path, "?")
    route = hd(parts)
    qs = if (length(parts) > 1) { hd(tl(parts)) } else { "" }
    case route {
        "/healthz"   -> http_respond(conn, 200, "Content-Type: text/plain\r\n", "ok\n")
        "/readyz"    -> _json(conn, _readyz())
        "/processes" -> _processes(conn, _int_param(qs, "limit", 0))
        "/tree"      -> _json(conn, Swarm.tree())
        "/top"       -> _top(conn, _param(qs, "by", "mailbox"), _int_param(qs, "n", 20))
        _            -> http_respond(conn, 404, "Content-Type: text/plain\r\n", "not found\n")
    }
}

fun _json(conn, value) {
    http_respond(conn, 200, "Content-Type: application/json\r\n", json_encode(value) ++ "\n")
}

fun _bad(conn, why) {
    http_respond(conn, 400, "Content-Type: text/plain\r\n", "bad request: " ++ why ++ "\n")
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

# One process_info call per pid; a process that exited between
# process_list() and process_info() (nil) is skipped, not reported as zeros.
fun _rows() {
    _rows_acc(process_list(), [])
}

fun _rows_acc(pids, acc) {
    if (length(pids) == 0) { _rev(acc, []) }
    else {
        info = process_info(hd(pids))
        if (info == nil) { _rows_acc(tl(pids), acc) }
        else { _rows_acc(tl(pids), [_row(info) | acc]) }
    }
}

fun _row(info) {
    %{pid: map_get(info, 'pid'), parent: map_get(info, 'parent'),
      name: map_get(info, 'name'), status: map_get(info, 'status'),
      mailbox_len: map_get(info, 'mailbox_len'), memory: map_get(info, 'memory'),
      resident_bytes: map_get(info, 'resident_bytes'),
      messages_sent: map_get(info, 'messages_sent'),
      messages_recv: map_get(info, 'messages_recv')}
}

fun _processes(conn, limit) {
    if (limit == nil) { _bad(conn, "limit must be a whole number >= 1") }
    else {
        rows = _rows()
        if (limit > 0 && length(rows) > limit) {
            _json(conn, _take(rows, limit, []) ++ [_summary(_drop(rows, limit))])
        } else { _json(conn, rows) }
    }
}

fun _summary(rows) {
    %{summary: 'true', omitted: length(rows),
      mailbox_len: _sum(rows, 'mailbox_len', 0), memory: _sum(rows, 'memory', 0),
      resident_bytes: _sum(rows, 'resident_bytes', 0),
      messages_sent: _sum(rows, 'messages_sent', 0),
      messages_recv: _sum(rows, 'messages_recv', 0)}
}

fun _sum(rows, key, acc) {
    if (length(rows) == 0) { acc } else { _sum(tl(rows), key, acc + map_get(hd(rows), key)) }
}

fun _top(conn, by, n) {
    key = case by {
        "mailbox"  -> 'mailbox_len'
        "memory"   -> 'memory'
        "resident" -> 'resident_bytes'
        "rate"     -> 'rate'
        _          -> nil
    }
    if (key == nil) { _bad(conn, "by must be mailbox, memory, resident or rate") }
    else {
        if (n == nil || n < 1) { _bad(conn, "n must be a whole number >= 1") }
        else {
            limit = if (n > 1000) { 1000 } else { n }
            _json(conn, map(fun(e) { elem(e, 1) }, _worst(_rows(), key, limit, [], 0)))
        }
    }
}

fun _score(r, key) {
    if (key == 'rate') { map_get(r, 'messages_sent') + map_get(r, 'messages_recv') }
    else { map_get(r, key) }
}

# _worst — bounded selection instead of sorting every process: `best` holds
# at most `limit` {score, row} pairs, highest score first, `cnt` of them.
# A row that can't beat the current floor once `best` is full is dropped
# after one comparison, so the common case is O(rows).
fun _worst(rows, key, limit, best, cnt) {
    if (length(rows) == 0) { best }
    else {
        r = hd(rows)
        sc = _score(r, key)
        if (cnt >= limit && sc <= elem(_last(best), 0)) { _worst(tl(rows), key, limit, best, cnt) }
        else {
            ins = _insert({sc, r}, best, [])
            if (cnt >= limit) { _worst(tl(rows), key, limit, _take(ins, limit, []), cnt) }
            else { _worst(tl(rows), key, limit, ins, cnt + 1) }
        }
    }
}

# Ties keep table order: a new entry goes after existing equal scores.
fun _insert(e, best, acc) {
    if (length(best) == 0) { _rev([e | acc], []) }
    else {
        if (elem(e, 0) > elem(hd(best), 0)) { _rev(acc, []) ++ [e | best] }
        else { _insert(e, tl(best), [hd(best) | acc]) }
    }
}

fun _last(lst) {
    if (length(tl(lst)) == 0) { hd(lst) } else { _last(tl(lst)) }
}

fun _take(lst, n, acc) {
    if (n <= 0 || length(lst) == 0) { _rev(acc, []) } else { _take(tl(lst), n - 1, [hd(lst) | acc]) }
}

fun _drop(lst, n) {
    if (n <= 0 || length(lst) == 0) { lst } else { _drop(tl(lst), n - 1) }
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

# An absent parameter gives `dflt`; a present one must be a plain decimal
# integer >= 1 ("12x", "0", "-3", "" give nil, which the caller turns into
# a 400 rather than guessing).
fun _int_param(qs, key, dflt) {
    v = _param(qs, key, nil)
    if (v == nil) { dflt }
    else {
        i = to_int(v)
        if (i == nil) { nil }
        else { if (to_string(i) == v && i >= 1) { i } else { nil } }
    }
}

fun _rev(lst, acc) {
    if (length(lst) == 0) { acc } else { _rev(tl(lst), [hd(lst) | acc]) }
}
