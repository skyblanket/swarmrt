# http_echo.sw — minimal HTTP server in 25 lines.
#
# Listens on localhost:8080. Every request gets routed through `case`
# on the path; responses use f-strings for easy templating. Hit with:
#
#   curl http://localhost:8080/
#   curl http://localhost:8080/hello/world
#   curl -X POST -d 'ping' http://localhost:8080/echo

module HttpEcho
export [main]

fun main() {
    http_listen(8080)
    print("listening on http://localhost:8080 (Ctrl-C to stop)")
    serve()
}

fun serve() {
    receive {
        {'http_request', conn, method, path, _hdrs, body} ->
            response = case path {
                "/"               -> "swarmrt http demo\n"
                "/hello"          -> "hello, swarm\n"
                p when string_starts_with(p, "/hello/") == 'true' ->
                    name = string_sub(p, 7, string_length(p) - 7)
                    f"hello, {name}\n"
                "/echo"           -> body
                _                 -> f"no route for {method} {path}\n"
            }
            http_respond(conn, 200, [], response)
            serve()
    }
}
