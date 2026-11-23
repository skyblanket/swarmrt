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
#   curl 'http://localhost:8080/top?by=mailbox&n=1'
#     → 200 [{"pid":..,"name":"backlog","mailbox_len":50,...}]
#                                         (the worst mailbox, ranked server-side)
#   also /processes[?limit=N] and /tree; see lib/Health.sw.
#
# PORT overrides the default 8080 (used by `make health-gate`).

module HealthEndpoint

import Health

export [main]

# A worker so /readyz has something to report besides main itself.
fun worker() {
    receive { 'stop' -> 'ok' }
}

# A stage that has stopped draining: it only takes 'stop', so the 'job'
# messages pile up and /top?by=mailbox names it.
fun backlog() {
    register("backlog", self())
    receive { 'stop' -> 'ok' }
}

fun flood(pid, n) {
    if (n > 0) { send(pid, {'job', n})  flood(pid, n - 1) } else { 'ok' }
}

fun main() {
    port = case getenv("PORT") { nil -> 8080  p -> to_int(p) }
    spawn(worker())
    b = spawn(backlog())
    flood(b, 50)
    print(f"health endpoint on http://localhost:{port} (/healthz, /readyz, /processes, /tree, /top) — Ctrl-C to stop")
    Health.serve(port)
}
