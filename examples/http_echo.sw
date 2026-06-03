# http_echo.sw — minimal HTTP server in ~30 lines.
#
# Listens on localhost:8080. Every request gets routed through `case`
# on the path; responses use f-strings for easy templating. The 5th
# element of {'http_request', ...} is the request-header MAP (lowercased
# keys), so bearer-token auth / webhook-signature reads work in-process.
# Hit with:
#
#   curl http://localhost:8080/
#   curl http://localhost:8080/hello/world
#   curl -X POST -d 'ping' http://localhost:8080/echo
#   curl -H 'Authorization: Bearer t0k' http://localhost:8080/whoami

module HttpEcho
export [main]

fun main() {
    http_listen(8080)
    print("listening on http://localhost:8080 (Ctrl-C to stop)")
    serve()
}

fun serve() {
    receive {
        {'http_request', conn, method, path, headers, body} ->
            response = case path {
                "/"               -> "swarmrt http demo\n"
                "/hello"          -> "hello, swarm\n"
                p when string_starts_with(p, "/hello/") == 'true' ->
                    name = string_sub(p, 7, string_length(p) - 7)
                    f"hello, {name}\n"
                "/echo"           -> body
                # Read the Authorization header out of the delivered MAP
                # (keys are lowercased; match either "..." or '...').
                "/whoami"         ->
                    case map_get(headers, "authorization") {
                        'nil' -> "no authorization header\n"
                        auth  -> f"{auth}\n"
                    }
                _                 -> f"no route for {method} {path}\n"
            }
            http_respond(conn, 200, [], response)
            serve()
    }
}
