module Test_http_bind

# Regression: http_listen(port) must bind 127.0.0.1, not every interface.
# It used to pass a NULL address to sw_tcp_listen (INADDR_ANY), so any dev
# server, agent control socket or LiveView page was reachable from the
# network the moment it started. Wider binds are opt-in:
#   http_listen(port, %{bind: "0.0.0.0"})   or   SW_HTTP_BIND=0.0.0.0
# (the option wins over the env var). A malformed bind address is an
# 'error', not a silent fallback to every interface.
#
# The listening address is read back from the kernel's socket table
# (/proc/net/tcp on Linux, `netstat -an` elsewhere). The SW_HTTP_BIND case
# runs in a child process so the env var is set before the runtime starts.

fun assert_true(name, cond, detail) {
    if (cond) { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name ++ ": " ++ detail) ; 1 }
}

# Is something LISTENing on addr:port? addr is "127.0.0.1" or "0.0.0.0";
# hexport is the port in upper-case hex for /proc/net/tcp.
fun listening(addr, port, hexport) {
    if (file_exists("/proc/net/tcp") == 'true') {
        # (via cat: /proc files report size 0, so file_read sees nothing)
        tab = elem(shell("cat /proc/net/tcp"), 1)
        hexaddr = if (addr == "127.0.0.1") { "0100007F" } else { "00000000" }
        string_contains(tab, hexaddr ++ ":" ++ hexport ++ " 00000000:0000 0A")
    } else {
        tab = elem(shell("netstat -an -p tcp 2>/dev/null | grep LISTEN"), 1)
        want = if (addr == "127.0.0.1") { "127.0.0.1." ++ port } else { "*." ++ port }
        string_contains(tab, want)
    }
}

fun serve() {
    receive {
        {'http_request', conn, _m, _p, _h, _b} -> http_respond(conn, 200, "", "ok") ; serve()
        _other -> serve()
    }
}

fun start(port, opts) {
    parent = self()
    spawn(fun() {
        r = if (opts == 'none') { http_listen(port) } else { http_listen(port, opts) }
        send(parent, {'listen', port, r})
        serve()
    })
    receive { {'listen', _p, r} -> r after 5000 -> 'timeout' }
}

# Child: SW_HTTP_BIND=0.0.0.0 is set in our environment.
fun child() {
    fails = 0
    fails = fails + assert_true("env_bind_listen_ok", start(9166, 'none') == 'ok', "http_listen(9166) failed")
    fails = fails + assert_true("env_bind_widens", listening("0.0.0.0", "9166", "23CE"),
                                "SW_HTTP_BIND=0.0.0.0 did not bind 0.0.0.0:9166")
    fails = fails + assert_true("opt_overrides_env", start(9167, %{bind: "127.0.0.1"}) == 'ok' &&
                                listening("127.0.0.1", "9167", "23CF"),
                                "bind: 127.0.0.1 did not win over SW_HTTP_BIND")
    sys_exit(fails)
}

fun main() {
    if (getenv("SW_HTTP_BIND_CHILD") == "1") { child() }
    fails = 0

    fails = fails + assert_true("default_listen_ok", start(9164, 'none') == 'ok', "http_listen(9164) failed")
    fails = fails + assert_true("default_is_loopback", listening("127.0.0.1", "9164", "23CC"),
                                "http_listen(9164) is not bound to 127.0.0.1")
    fails = fails + assert_true("default_not_wildcard", listening("0.0.0.0", "9164", "23CC") == false,
                                "http_listen(9164) is bound to every interface")
    body = http_get("http://127.0.0.1:9164/x")
    fails = fails + assert_true("loopback_serves", body == "ok", "GET over loopback got " ++ to_string(body))

    fails = fails + assert_true("opt_bind_all", start(9165, %{bind: "0.0.0.0"}) == 'ok' &&
                                listening("0.0.0.0", "9165", "23CD"),
                                "bind: 0.0.0.0 did not bind every interface")
    fails = fails + assert_true("bad_bind_is_error", start(9169, %{bind: "not-an-address"}) == 'error',
                                "a malformed bind address did not fail")

    me = hd(os_args())
    r = shell("SW_HTTP_BIND_CHILD=1 SW_HTTP_BIND=0.0.0.0 '" ++ me ++ "' 2>&1")
    print(string_trim(elem(r, 1)))
    if (elem(r, 0) != 0) { fails = fails + 1 }

    if (fails == 0) { print("OK http_bind 9/9") ; sys_exit(0) }
    else { print("FAIL http_bind") ; sys_exit(1) }
}
