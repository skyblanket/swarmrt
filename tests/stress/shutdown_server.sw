# shutdown_server.sw — a long-lived "server" for the graceful-shutdown gate
# (tests/stress/shutdown_gate.sh, Phase 4). It never returns from main() on
# its own — it parks in a receive forever, so the ONLY way it ends is a
# SIGTERM/SIGINT triggering sw_shutdown_graceful (drain-with-deadline).
#
# Modes (argv[1]):
#   idle  — spawn a few workers that park in receive (empty mailboxes). The
#           node is QUIESCENT, so a SIGTERM drains instantly and the process
#           exits fast within the deadline ("drained in Nms" on stderr).
#   busy  — additionally spawn a worker in an infinite tail-recursive loop
#           (never parks → never quiescent). A SIGTERM can NOT drain it, so
#           the deadline must FORCE teardown and the process must still exit
#           (bounded — the scheduler join honours should_exit at the next
#           reduction boundary). Proves the deadline bounds a hung workload.
module Main

# Parks forever with an empty mailbox — a quiescent worker.
fun idle_worker() {
    receive {
        'stop' -> 0
    }
}

# Never parks: infinite tail loop, always runnable. Honours should_exit only
# via scheduler preemption at reduction boundaries (that's the bound we test).
fun busy_worker(n) {
    busy_worker(n + 1)
}

fun spawn_idle(k) {
    if (k <= 0) { 0 }
    else {
        spawn(idle_worker())
        spawn_idle(k - 1)
    }
}

fun main() {
    mode = case os_args() {
        [_prog, m | _rest] -> m
        _ -> "idle"
    }
    spawn_idle(3)
    case mode {
        "busy" -> spawn(busy_worker(0))
        _ -> 0
    }
    # Signal readiness so the gate knows when to fire SIGTERM (avoids racing
    # the spawn), then park forever — only a signal ends this.
    print("SHUTDOWN_SERVER_READY")
    receive {
        'never' -> 0
    }
}
