module Test_llm_endpoint

# llm_complete / llm_stream have no built-in endpoint: a program says where
# its prompts go (opts.url, LLM_URL, or a provider), and without one both
# fail with a message naming those settings. They used to fall back to a
# hosted vendor proxy, so an unconfigured program quietly sent its prompts
# off the machine; llm_stream also ignored LLM_URL entirely.
#
# Each case re-runs this binary with its own environment and an in-process
# OpenAI-compatible echo server; the server's reply carries the path, the
# Authorization header and the model the request arrived with.

fun serve(port) {
    http_listen(port)
    serve_loop()
}

fun serve_loop() {
    receive {
        {'http_request', conn, _method, path, headers, body} ->
            auth = case map_get(headers, "authorization") { 'nil' -> "<none>" v -> v }
            req = json_decode(body)
            model = if (req == nil) { "<badjson>" }
                    else { case map_get(req, 'model') { 'nil' -> "<none>" m -> m } }
            echo = path ++ "|" ++ auth ++ "|" ++ to_string(model)
            if (string_contains(body, "\"stream\":true") == 'true') {
                http_respond(conn, 200, "Content-Type: text/event-stream\r\n",
                    "data: " ++ json_encode(%{choices: [%{delta: %{content: echo}}]}) ++ "\n\n" ++
                    "data: [DONE]\n\n")
            } else {
                http_respond(conn, 200, "Content-Type: application/json\r\n",
                    json_encode(%{choices: [%{message: %{content: echo}}]}))
            }
            serve_loop()
        _other -> serve_loop()
    }
}

fun wait_listening(port, tries) {
    r = http_get("http://127.0.0.1:" ++ port ++ "/ping", [])
    if (r != nil || tries == 0) { 'ok' } else { sleep(50) ; wait_listening(port, tries - 1) }
}

fun stream_text(opts) {
    llm_stream("hi", opts)
    receive {
        {'llm_done', text} -> text
        after 10000 -> "<timeout>"
    }
}

fun child(which) {
    port = getenv("SW_LLM_PORT")
    spawn(serve(to_int(port)))
    wait_listening(port, 60)
    base = "http://127.0.0.1:" ++ port
    out = case which {
        "none"      -> llm_complete("hi") ++ "\n" ++ stream_text(%{})
        "badprov"   -> llm_complete("hi")
        "opts_url"  -> llm_complete("hi", %{url: base ++ "/opt"})
        "env_url"   -> llm_complete("hi") ++ "\n" ++ stream_text(%{})
        "ollama"    -> llm_complete("hi")
        "keyroute"  -> llm_complete("hi")
        "model_esc" -> llm_complete("hi", %{url: base ++ "/m", model: "a\"b"})
        _           -> "unknown case"
    }
    print(out)
    sys_exit(0)
}

fun run_case(me, which, port, env) {
    clear = "env -u LLM_URL -u LLM_PROVIDER -u LLM_MODEL -u LLM_API_KEY -u OLLAMA_HOST " ++
            "-u OPENAI_API_KEY -u OTONOMY_API_KEY"
    cmd = clear ++ " SW_LLM_CASE=" ++ which ++ " SW_LLM_PORT=" ++ port ++ " SW_SCHEDULERS=4 " ++
          env ++ " '" ++ me ++ "' 2>/dev/null"
    string_trim(elem(shell(cmd), 1))
}

# The same run, keeping only stderr.
fun run_case_stderr(me, which, port, env) {
    clear = "env -u LLM_URL -u LLM_PROVIDER -u LLM_MODEL -u LLM_API_KEY -u OLLAMA_HOST " ++
            "-u OPENAI_API_KEY -u OTONOMY_API_KEY"
    cmd = clear ++ " SW_QUIET=1 SW_LLM_CASE=" ++ which ++ " SW_LLM_PORT=" ++ port ++ " SW_SCHEDULERS=4 " ++
          env ++ " '" ++ me ++ "' 2>&1 >/dev/null"
    string_trim(elem(shell(cmd), 1))
}

fun check(name, cond, got) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name ++ ": got " ++ to_string(got)) ; 1 }
}

fun has(s, sub) { string_contains(s, sub) }

fun line(ls, i) {
    if (length(ls) == 0) { "" } else { if (i == 0) { hd(ls) } else { line(tl(ls), i - 1) } }
}

fun both(a, b) { if (a == 'true' && b == 'true') { 'true' } else { 'false' } }

fun main() {
    which = getenv("SW_LLM_CASE")
    if (which != nil) { child(which) }
    else {
        me = hd(os_args())
        f = 0

        none = run_case(me, "none", "9261", "")
        lines = string_split(none, "\n")
        f = f + check("no_endpoint_complete_fails_clearly",
                      both(string_starts_with(line(lines, 0), "error: llm_complete: no LLM endpoint configured"),
                           has(line(lines, 0), "LLM_URL")), none)
        f = f + check("no_endpoint_stream_fails_clearly",
                      string_starts_with(line(lines, 1), "error: llm_stream: no LLM endpoint configured"), none)

        # Also on stderr — once, although the case calls both builtins.
        err = run_case_stderr(me, "none", "9268", "")
        f = f + check("no_endpoint_reported_once_on_stderr",
                      both(string_starts_with(err, "swarmrt: error: llm_complete: no LLM endpoint configured"),
                           if (length(string_split(err, "\n")) == 1) { 'true' } else { 'false' }), err)

        bad = run_case(me, "badprov", "9262", "LLM_PROVIDER=nosuch")
        f = f + check("unknown_provider_named", has(bad, "unknown provider 'nosuch'"), bad)

        o = run_case(me, "opts_url", "9263", "")
        f = f + check("opts_url_used_model_left_out", o == "/opt|Bearer ollama|<none>", o)

        e = run_case(me, "env_url", "9264",
                     "LLM_URL=http://127.0.0.1:9264/env LLM_MODEL=m1 LLM_API_KEY=k1")
        el = string_split(e, "\n")
        f = f + check("llm_url_used_by_complete", line(el, 0) == "/env|Bearer k1|m1", e)
        f = f + check("llm_url_used_by_stream", line(el, 1) == "/env|Bearer k1|m1", e)

        ol = run_case(me, "ollama", "9265", "LLM_PROVIDER=ollama OLLAMA_HOST=127.0.0.1:9265/")
        f = f + check("ollama_provider_uses_ollama_host", string_starts_with(ol, "/v1/chat/completions|"), ol)

        k = run_case(me, "keyroute", "9266",
                     "OPENAI_API_KEY=sk-leak OTONOMY_API_KEY=ot-leak " ++
                     "'LLM_URL=http://127.0.0.1:9266/x?u=https://api.openai.com/&otonomy-inference-production.up.railway.app'")
        f = f + check("provider_keys_stay_with_their_provider",
                      both(has(k, "Bearer ollama"),
                           if (has(k, "leak") == 'true') { 'false' } else { 'true' }), k)

        m = run_case(me, "model_esc", "9267", "")
        f = f + check("model_name_is_json_escaped", m == "/m|Bearer ollama|a\"b", m)

        if (f == 0) { print("OK llm_endpoint 10/10") ; sys_exit(0) }
        else { print("FAIL llm_endpoint") ; sys_exit(1) }
    }
}
