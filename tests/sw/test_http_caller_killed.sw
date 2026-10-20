module Test_http_caller_killed

# A caller killed while it waits on a hung HTTP request must take that request
# down with it. Before this, llm_complete's curl child ran until its
# --max-time (300 s) and held an offload thread the whole time. A caller
# killed by a task_stream deadline (exit_proc) now has its curl killed within
# about one poll tick. opts.timeout_ms bounds one request on its own.
#
# An in-VM server accepts every request and never answers it. The curl
# children aimed at its port are counted with pgrep; the bracket patterns
# keep the shell running that count from matching itself.

import Std

fun hung_server() {
    http_listen(9181)
    hung_loop()
}

fun hung_loop() {
    receive {
        {'http_request', _conn, _method, _path, _headers, _body} -> hung_loop()
        _other -> hung_loop()
    }
}

fun curl_left() {
    r = shell("pgrep -f '[c]url .*127[.]0[.]0[.]1:9181' | wc -l")
    string_trim(elem(r, 1))
}

# Poll up to ticks x 100 ms for the curl children to be gone.
fun wait_curl_gone(ticks) {
    if (curl_left() == "0") { "0" }
    else if (ticks <= 0) { curl_left() }
    else { sleep(100) ; wait_curl_gone(ticks - 1) }
}

fun hung_llm(opts) {
    llm_complete("hi", opts)
}

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun main() {
    spawn(hung_server())
    sleep(300)
    url = "http://127.0.0.1:9181/v1/chat/completions"
    opts = %{url: url, api_key: "k", model: "m"}
    fails = 0

    # 1. A task_stream deadline kills the worker blocked in llm_complete.
    #    Its curl must be gone within ~1 s of the kill.
    r = Std.task_stream([1], fun(_x) { hung_llm(opts) }, %{timeout_ms: 1000})
    fails = fails + assert_eq("deadline_reported", r, [{'error', 'timeout'}])
    left = wait_curl_gone(10)
    fails = fails + assert_eq("killed_caller_curl_gone", left, "0")

    # 2. opts.timeout_ms bounds a single request: a timed-out llm_complete
    #    returns the timeout error in about the given time, not 300 s.
    t0 = timestamp()
    t = llm_complete("hi", %{url: url, api_key: "k", model: "m", timeout_ms: 500})
    elapsed = timestamp() - t0
    fails = fails + assert_eq("timeout_error", t, "error: timeout")
    if (elapsed < 1500) { print("PASS timeout_prompt") }
    else { print("FAIL timeout_prompt: " ++ to_string(elapsed) ++ "ms") ; fails = fails + 1 }

    # 3. The same bound applies to each HTTP builtin's opts.
    p = http_post(url, [], "{}", %{timeout_ms: 500})
    fails = fails + assert_eq("http_post_timeout", p, "error: timeout")
    g = http_get(url, [], %{timeout_ms: 500})
    fails = fails + assert_eq("http_get_timeout", g, "error: timeout")
    q = http_request(url, %{method: "POST", body: "{}", timeout_ms: 500})
    fails = fails + assert_eq("http_request_timeout", q, {'error', "http_request: timeout"})
    s = llm_stream("hi", %{url: url, api_key: "k", model: "m", timeout_ms: 500})
    s_done = receive {
        {'llm_done', text} -> text
        after 5000 -> 'none'
    }
    fails = fails + assert_eq("llm_stream_timeout", s_done, "error: timeout")

    if (fails == 0) { print("OK http_caller_killed 8/8") ; sys_exit(0) }
    else { print("FAIL http_caller_killed") ; sys_exit(1) }
}
