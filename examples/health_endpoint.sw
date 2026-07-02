# health_endpoint.sw — operator liveness/readiness surface in three lines
# of application code (lib/Health.sw does the rest).
#
#   swc build examples/health_endpoint.sw -o bin/health_demo && ./bin/health_demo
#
#   curl http://localhost:8080/healthz
#     → 200 "ok"                          (liveness: the scheduler is alive)
#   curl http://localhost:8080/readyz
#     → 200 {"processes":3,"schedulers":8,"spawns":2,"crashes":0,...}
#                                         (readiness: live swarm_stats() JSON)
#
# PORT overrides the default 8080 (used by `make health-gate`).

module HealthEndpoint

import Health

export [main]

# A worker so /readyz has something to report besides main itself.
fun worker() {
    receive { 'stop' -> 'ok' }
}

fun main() {
    port = case getenv("PORT") { nil -> 8080  p -> to_int(p) }
    spawn(worker())
    print(f"health endpoint on http://localhost:{port} (/healthz, /readyz) — Ctrl-C to stop")
    Health.serve(port)
}
