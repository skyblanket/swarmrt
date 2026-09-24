module Test_http_fd_leak

# Regression: the HTTP server must not leak a file descriptor per
# connection. When a keep-alive client hung up, the bridge freed its
# connection slot but never closed the socket, so every request leaked one
# fd until accept() failed with EMFILE (~1000 requests on a default
# `ulimit -n`). 200 sequential requests must leave the fd count flat.

import Std

fun server() {
    http_listen(9153)
    serve()
}

fun serve() {
    receive {
        {'http_request', conn, _m, _p, _h, _b} -> http_respond(conn, 200, "", "ok") ; serve()
        _other -> serve()
    }
}

fun wait_up(attempts) {
    r = http_request("http://127.0.0.1:9153/ping", %{})
    case r {
        {'error', _} -> if (attempts <= 1) { 'down' } else { sleep(50) ; wait_up(attempts - 1) }
        _ -> 'up'
    }
}

fun hammer(i, n, ok) {
    if (i == n) { ok }
    else {
        body = http_get("http://127.0.0.1:9153/x")
        hammer(i + 1, n, if (body == "ok") { ok + 1 } else { ok })
    }
}

fun open_fds() { length(file_list("/dev/fd")) }

fun main() {
    spawn(server())
    if (wait_up(100) != 'up') { print("FAIL server_up") ; sys_exit(1) }
    sleep(200)
    fds_before = open_fds()
    ok = hammer(0, 200, 0)
    sleep(300)
    fds_after = open_fds()
    fails = 0
    if (ok == 200) { print("PASS all_ok") } else { print(f"FAIL all_ok: {ok}/200") ; fails = 1 }
    if (fds_after - fds_before < 20) { print("PASS fds_flat") }
    else { print(f"FAIL fds_flat: {fds_before} -> {fds_after} open fds after 200 requests") ; fails = fails + 1 }
    if (fails == 0) { print("OK http_fd_leak 2/2") ; sys_exit(0) }
    else { print("FAIL http_fd_leak") ; sys_exit(1) }
}
