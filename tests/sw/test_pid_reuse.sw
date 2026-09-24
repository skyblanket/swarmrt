module Test_pid_reuse

# Regression: a pid must keep denoting the process it was made for. Process
# slots are recycled; a pid value used to hold only the slot pointer, so once
# a dead process's slot was reused its stale pid compared == to the new
# occupant, send() delivered to it, exit_proc() KILLED it and monitor()
# watched it. Pids now carry the numeric id; a stale pid is dead.
#
# Slot reuse is deterministic on one scheduler, so the probe re-runs this
# binary with SW_SCHEDULERS=1 and checks its report.

fun short() { 'bye' }

fun victim(parent) {
    receive {
        'ping' -> send(parent, 'victim_got_ping') ; victim(parent)
        'stop' -> 'ok'
        after 3000 -> 'ok'
    }
}

fun probe() {
    me = self()
    dead = spawn(short())
    sleep(50)
    v = spawn(victim(me))
    print(f"equal={dead == v}")
    send(dead, 'ping')
    r = receive { 'victim_got_ping' -> "misdelivered" after 300 -> "dropped" }
    print(f"send={r}")
    exit_proc(dead, 'kill')
    send(v, 'ping')
    r2 = receive { 'victim_got_ping' -> "alive" after 300 -> "killed" }
    print(f"victim={r2}")
    monitor(dead)
    d = receive { {'DOWN', _r, _p, _pid, reason} -> reason after 300 -> 'none' }
    print(f"down={d}")
    send(v, 'stop')
}

fun main() {
    args = os_args()
    if (length(args) > 1) { probe() }
    else {
        out = elem(shell("SW_SCHEDULERS=1 " ++ hd(args) ++ " probe"), 1)
        want = "equal=false\nsend=dropped\nvictim=alive\ndown=noproc\n"
        if (out == want) { print("PASS pid_reuse") ; print("OK pid_reuse 1/1") ; sys_exit(0) }
        else { print("FAIL pid_reuse: got " ++ out) ; sys_exit(1) }
    }
}
