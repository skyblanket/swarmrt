# Health.sw — liveness/readiness endpoint for operators, pure sw over the
# http_listen/http_respond builtins + the Phase-4 swarm_stats() metrics map.
#
#   import Health
#   Health.start(8080)        # spawn the server, keep working
#   # ...or Health.serve(8080) to run it in the calling process (blocks)
#
#   curl http://localhost:8080/healthz   → 200 "ok"        (liveness)
#   curl http://localhost:8080/readyz    → 200 swarm_stats() as JSON (readiness)
#
# /healthz answers 200 the moment the scheduler can run this process — a
# liveness probe. /readyz additionally serializes the live swarm_stats()
# map (process count, spawns/crashes/restarts, drop counters, per-scheduler
# stats), so an operator or readiness probe sees real runtime state.
# Anything else is 404. COMPILED-ONLY in practice: http_listen needs the
# scheduler + IO bridge of a `swc build` binary.

module Health

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
        "/readyz"  -> http_respond(conn, 200, "Content-Type: application/json\r\n",
                                   json_encode(swarm_stats()) ++ "\n")
        _          -> http_respond(conn, 404, "Content-Type: text/plain\r\n", "not found\n")
    }
}
