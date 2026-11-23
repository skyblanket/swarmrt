module Test_node_tls

# Distribution over TLS (SW_NODE_TLS=1, #40), across real OS processes.
# alpha and beta share SW_NODE_COOKIE: alpha's message reaches beta and
# beta's reply comes back over the encrypted link. gamma has a different
# cookie: its node_connect to beta must fail (the TLS handshake cannot
# complete), not report 'ok' for a link that silently drops everything.
#
# Re-runs itself as three OS processes on local ports 19721-19723.

fun connect_retry(node, port, tries) {
    r = node_connect(node, "127.0.0.1", port)
    if (r == 'ok') { 'ok' }
    else {
        if (tries == 0) { r }
        else {
            sleep(100)
            connect_retry(node, port, tries - 1)
        }
    }
}

fun alpha() {
    node_start("alpha@127.0.0.1", 19721)
    connect_retry("beta@127.0.0.1", 19722, 50)
    node_send("beta@127.0.0.1", "echo", {'ping', self(), "over tls"})
    r = receive {
        {'pong', "over tls"} -> 'ok'
        after 8000 -> 'timeout'
    }
    print(f"RESULT {r}")
    sys_exit(0)
}

fun beta() {
    register("echo", self())
    node_start("beta@127.0.0.1", 19722)
    receive {
        {'ping', from, text} -> send(from, {'pong', text})
        after 8000 -> 'timeout'
    }
    sleep(3000)
    sys_exit(0)
}

fun gamma() {
    node_start("gamma@127.0.0.1", 19723)
    sleep(500)
    r = node_connect("beta@127.0.0.1", "127.0.0.1", 19722)
    print(f"GAMMA {r}")
    sys_exit(0)
}

fun await_line(path, marker, tries) {
    out = elem(shell("cat '" ++ path ++ "' 2>/dev/null"), 1)
    if (string_contains(out, marker)) { out }
    else {
        if (tries == 0) { out }
        else {
            sleep(200)
            await_line(path, marker, tries - 1)
        }
    }
}

fun launch(role, cookie, bin, dir) {
    shell("SW_NODE_TLS=1 SW_NODE_COOKIE=" ++ cookie ++ " SW_NODE_ROLE=" ++ role ++ " '" ++ bin ++ "' > '" ++ dir ++ "/" ++ role ++ ".log' 2>&1 &")
}

fun check(name, ok, detail) {
    if (ok) { print("PASS " ++ name) 1 }
    else { print("FAIL " ++ name ++ ": " ++ detail) 0 }
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
                launch("beta", "shared-cookie-4f1d", bin, dir)
                launch("alpha", "shared-cookie-4f1d", bin, dir)
                launch("gamma", "other-cookie-9e2b", bin, dir)
                a = await_line(dir ++ "/alpha.log", "RESULT ", 100)
                g = await_line(dir ++ "/gamma.log", "GAMMA ", 100)
                logs = elem(shell("cat '" ++ dir ++ "'/*.log 2>/dev/null"), 1)
                shell("rm -rf '" ++ dir ++ "'")
                if (string_contains(logs, "this build has no OpenSSL")) {
                    # macOS default build (no SWARMRT_TLS=1): TLS is refused
                    # loudly by design; nothing more to check here.
                    print("SKIP node_tls: built without OpenSSL")
                    print("OK node_tls 0/0")
                    sys_exit(0)
                }
                n = check("tls_round_trip", string_contains(a, "RESULT ok"), a ++ logs)
                n = n + check("tls_wrong_cookie_refused",
                              string_contains(g, "GAMMA ") && string_contains(g, "GAMMA ok") == false, g ++ logs)
                if (n == 2) {
                    print("OK node_tls 2/2")
                    sys_exit(0)
                } else {
                    print("FAIL node_tls")
                    sys_exit(1)
                }
            }
        }
    }
}
