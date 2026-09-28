module Test_ws_origin

# Regression: a WebSocket upgrade from a foreign browser Origin must be
# refused (403). Browsers do not apply the same-origin policy to
# WebSockets and attach this host's cookies, so without the check any web
# page a user visits could open a socket to a local http_listen server
# (cross-site WebSocket hijacking). The server used to upgrade every
# request regardless of Origin.
#
# Allowed: no Origin at all (non-browser clients — wsc_connect, curl, a
# telephony provider), a loopback Origin, a same-origin request (Origin
# authority == Host), and anything in http_listen's ws_origins option or
# the SW_WS_ORIGINS env var. Everything else, including "null", gets 403.
#
# The upgrades are sent with curl (wsc_connect has no way to set Origin):
# a 101 keeps the socket open, so the allowed cases run under --max-time 1
# and we read the status curl saw. curl blocks a scheduler thread, so the
# body runs in a child with SW_SCHEDULERS=2 (the server keeps one) and with
# SW_WS_ORIGINS set before the runtime starts.

fun serve() {
    receive {
        {'ws_connect', _conn, _path} -> serve()
        {'http_request', conn, _m, _p, _h, _b} -> http_respond(conn, 200, "", "ok") ; serve()
        _other -> serve()
    }
}

fun upgrade(host, origin) {
    base = "curl -s -o /dev/null -w '%{http_code}' --max-time 1" ++
           " -H 'Connection: Upgrade' -H 'Upgrade: websocket'" ++
           " -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=='"
    h = if (host == "") { "" } else { " -H 'Host: " ++ host ++ "'" }
    o = if (origin == "") { "" } else { " -H 'Origin: " ++ origin ++ "'" }
    string_trim(elem(shell(base ++ h ++ o ++ " http://127.0.0.1:9168/ws 2>/dev/null"), 1))
}

fun check(name, host, origin, want) {
    got = upgrade(host, origin)
    if (got == want) { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name ++ ": Origin '" ++ origin ++ "' got " ++ got ++ ", want " ++ want) ; 1 }
}

fun wait_up(attempts) {
    r = http_request("http://127.0.0.1:9168/ping", %{})
    case r {
        {'error', _} -> if (attempts <= 1) { 'down' } else { sleep(50) ; wait_up(attempts - 1) }
        _ -> 'up'
    }
}

fun child() {
    spawn(fun() {
        http_listen(9168, %{ws_origins: ["https://app.example", "https://two.example/"]})
        serve()
    })
    if (wait_up(100) != 'up') { print("FAIL server_up") ; sys_exit(1) }
    fails = 0
    fails = fails + check("no_origin_allowed", "", "", "101")
    fails = fails + check("foreign_origin_refused", "", "https://evil.example", "403")
    fails = fails + check("null_origin_refused", "", "null", "403")
    fails = fails + check("lookalike_refused", "", "https://app.example.evil.example", "403")
    fails = fails + check("opt_origin_allowed", "", "https://app.example", "101")
    fails = fails + check("opt_trailing_slash", "", "https://two.example", "101")
    fails = fails + check("env_origin_allowed", "", "https://env.example", "101")
    fails = fails + check("loopback_origin_allowed", "", "http://localhost:3000", "101")
    fails = fails + check("same_origin_allowed", "chat.example:9168", "https://chat.example:9168", "101")
    fails = fails + check("cross_port_refused", "chat.example:9168", "https://chat.example:4444", "403")
    sys_exit(fails)
}

fun main() {
    if (getenv("SW_WS_ORIGIN_CHILD") == "1") { child() }
    me = hd(os_args())
    r = shell("SW_WS_ORIGIN_CHILD=1 SW_SCHEDULERS=2 SW_WS_ORIGINS='https://other.example, https://env.example' '" ++
              me ++ "' 2>/dev/null")
    print(string_trim(elem(r, 1)))
    if (elem(r, 0) == 0) { print("OK ws_origin 10/10") ; sys_exit(0) }
    else { print("FAIL ws_origin") ; sys_exit(1) }
}
