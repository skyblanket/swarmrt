module Test_node_forward

# Regression: a pid keeps its owning node when it is forwarded. Node A sends
# its own pid to B; B forwards the value it received to C; C's send(p, ...)
# must reach A. The forwarded pid used to be re-encoded with the forwarder's
# name, so C routed the message to B.
#
# Re-runs itself as three OS processes (one node each) on local ports
# 19711-19713 and checks the report A prints. C connects to A directly, so
# C can route replies to A.

fun connect_retry(node, port, tries) {
    if (node_connect(node, "127.0.0.1", port) == 'ok') { 'ok' }
    else {
        if (tries == 0) { 'error' }
        else {
            sleep(100)
            connect_retry(node, port, tries - 1)
        }
    }
}

fun alpha() {
    node_start("alpha@127.0.0.1", 19711)
    connect_retry("beta@127.0.0.1", 19712, 50)
    node_send("beta@127.0.0.1", "relay", {'pid', self()})
    r = receive {
        {'hello', _w} -> 'ok'
        after 8000 -> 'timeout'
    }
    print(f"RESULT {r}")
    sys_exit(0)
}

fun beta() {
    register("relay", self())
    node_start("beta@127.0.0.1", 19712)
    connect_retry("gamma@127.0.0.1", 19713, 50)
    receive {
        {'pid', p} -> node_send("gamma@127.0.0.1", "sink", {'fwd', p})
        after 8000 -> 'timeout'
    }
    sleep(1500)
    sys_exit(0)
}

fun gamma() {
    node_start("gamma@127.0.0.1", 19713)
    register("sink", self())
    connect_retry("alpha@127.0.0.1", 19711, 50)
    receive {
        {'fwd', p} -> send(p, {'hello', 'from_gamma'})
        after 8000 -> 'timeout'
    }
    sleep(1500)
    sys_exit(0)
}

fun await_result(path, tries) {
    out = elem(shell("cat '" ++ path ++ "' 2>/dev/null"), 1)
    if (string_contains(out, "RESULT ")) { out }
    else {
        if (tries == 0) { out }
        else {
            sleep(200)
            await_result(path, tries - 1)
        }
    }
}

fun launch(role, bin, dir) {
    shell("SW_NODE_ROLE=" ++ role ++ " '" ++ bin ++ "' > '" ++ dir ++ "/" ++ role ++ ".log' 2>&1 &")
}

fun main() {
    role = getenv("SW_NODE_ROLE")
    if (role == "alpha") { alpha() }
    else {
        if (role == "beta") { beta() }
        else {
            if (role == "gamma") { gamma() }
            else {
                bin = hd(os_args())
                dir = string_trim(elem(shell("mktemp -d"), 1))
                launch("gamma", bin, dir)
                launch("beta", bin, dir)
                launch("alpha", bin, dir)
                out = await_result(dir ++ "/alpha.log", 100)
                logs = elem(shell("cat '" ++ dir ++ "'/*.log 2>/dev/null"), 1)
                shell("rm -rf '" ++ dir ++ "'")
                if (string_contains(out, "RESULT ok")) {
                    print("PASS forwarded_pid_reaches_owner")
                    print("OK node_forward 1/1")
                    sys_exit(0)
                } else {
                    print("FAIL forwarded_pid_reaches_owner: " ++ out ++ logs)
                    print("FAIL node_forward")
                    sys_exit(1)
                }
            }
        }
    }
}
