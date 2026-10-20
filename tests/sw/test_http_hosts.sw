module Test_http_hosts

# Regression: the WebSocket same-origin rule trusts Host, and a DNS-rebinding
# page controls Host and Origin together, so it passes that rule. With
# SW_HTTP_HOSTS set, every request whose Host is not listed gets 403 before
# the upgrade or the handler runs. Unset, nothing changes (test_ws_origin.sw).
#
# Covered: a listed Host upgrades and serves, a rebinding upgrade
# (Host: evil.test, Origin: http://evil.test) is refused, the same with no
# Origin, plain HTTP with a foreign Host is refused, and the match is exact
# (port included, case-insensitive).
#
# Like test_ws_origin.sw, the upgrades go through curl (a 101 keeps the socket
# open, so it runs under --max-time 1), and the body runs in a child with
# SW_HTTP_HOSTS set before the runtime starts.

fun serve() {
    receive {
        {'ws_connect', _conn, _path} -> serve()
        {'http_request', conn, _m, _p, _h, _b} -> http_respond(conn, 200, "", "ok") ; serve()
        _other -> serve()
    }
}

fun curl_code(extra) {
    string_trim(elem(shell("curl -s -o /dev/null -w '%{http_code}' --max-time 1" ++ extra ++ " 2>/dev/null"), 1))
}

fun plain(host) {
    curl_code(" -H 'Host: " ++ host ++ "' http://127.0.0.1:9172/ping")
}

fun upgrade(host, origin) {
    base = " -H 'Connection: Upgrade' -H 'Upgrade: websocket'" ++
           " -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=='"
    o = if (origin == "") { "" } else { " -H 'Origin: " ++ origin ++ "'" }
    curl_code(base ++ " -H 'Host: " ++ host ++ "'" ++ o ++ " http://127.0.0.1:9172/ws")
}

fun check(name, got, want) {
    if (got == want) { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name ++ ": got " ++ got ++ ", want " ++ want) ; 1 }
}

fun wait_up(attempts) {
    r = plain("app.test:9172")
    if (r == "200") { 'up' }
    else { if (attempts <= 1) { 'down' } else { sleep(50) ; wait_up(attempts - 1) } }
}

fun child() {
    spawn(fun() {
        http_listen(9172, %{})
        serve()
    })
    if (wait_up(100) != 'up') { print("FAIL server_up") ; sys_exit(1) }
    fails = 0
    fails = fails + check("listed_http_ok", plain("app.test:9172"), "200")
    fails = fails + check("listed_second_entry_ok", plain("localhost:9172"), "200")
    fails = fails + check("listed_case_insensitive_ok", plain("APP.test:9172"), "200")
    fails = fails + check("rebind_http_refused", plain("evil.test"), "403")
    fails = fails + check("port_mismatch_refused", plain("app.test:9999"), "403")
    fails = fails + check("listed_upgrade_ok", upgrade("app.test:9172", "http://app.test:9172"), "101")
    fails = fails + check("rebind_upgrade_refused", upgrade("evil.test", "http://evil.test"), "403")
    fails = fails + check("rebind_no_origin_refused", upgrade("evil.test", ""), "403")
    sys_exit(fails)
}

fun main() {
    if (getenv("SW_HTTP_HOSTS_CHILD") == "1") { child() }
    me = hd(os_args())
    r = shell("SW_HTTP_HOSTS_CHILD=1 SW_SCHEDULERS=2 SW_HTTP_HOSTS='app.test:9172, localhost:9172' '" ++
              me ++ "' 2>/dev/null")
    print(string_trim(elem(r, 1)))
    if (elem(r, 0) == 0) { print("OK http_hosts 8/8") ; sys_exit(0) }
    else { print("FAIL http_hosts") ; sys_exit(1) }
}
