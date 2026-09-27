module Agent_test

# Runs offline: an in-process server plays the model. It answers the tool
# prompt with a tool call and everything else directly. The "crash" and
# "hang" tasks check that one bad agent costs one result, not the run.

import Std
import Agent
import Tools

fun mock(port) { http_listen(port) ; mock_loop() }

fun mock_loop() {
    receive {
        {'http_request', conn, _m, _p, _h, body} ->
            spawn(respond(conn, body))    # one process per request: a slow one blocks only itself
            mock_loop()
        _ -> mock_loop()
    }
}

fun reply_to(prompt) {
    if (string_contains(prompt, "Tool result: 4")) { "4 words" }
    else { if (string_contains(prompt, "count the words in: a b c d")) { "CALL word_count a b c d" }
    else { if (string_contains(prompt, "Task: hang")) { sleep(60000) ; "late" }
    else { if (string_contains(prompt, "Task: crash")) { "error: model refused" }
    else { "pong" }}}}
}

fun respond(conn, body) {
    req = json_decode(body)
    reply = if (req == nil) { "up" } else { reply_to(map_get(hd(map_get(req, 'messages')), 'content')) }
    http_respond(conn, 200, "Content-Type: application/json\r\n",
                 json_encode(%{choices: [%{message: %{content: reply}}]}))
}

fun check(name, cond, got) {
    if (cond) { print("PASS " ++ name) ; 0 } else { print("FAIL " ++ name ++ ": " ++ to_string(got)) ; 1 }
}

fun wait_up(url, n) {
    if (http_get(url, []) != nil || n == 0) { 'ok' } else { sleep(50) ; wait_up(url, n - 1) }
}

fun main() {
    spawn(mock(18431))
    base = "http://127.0.0.1:18431"
    wait_up(base ++ "/", 60)
    llm = %{url: base ++ "/v1/chat/completions"}
    f = check("tool_word_count", Tools.word_count("  a b  c ") == "3", Tools.word_count("  a b  c "))
    f = f + check("answers_directly", Agent.answer("ping", llm) == "pong", "")
    f = f + check("uses_the_tool", Agent.answer("count the words in: a b c d", llm) == "4 words", "")
    tasks = ["ping", "crash", "hang", "ping"]
    rs = Std.task_stream(tasks, fn(t) { Agent.answer(t, llm) }, %{max_concurrency: 4, timeout_ms: 2000})
    oks = length(filter(rs, fn(r) { r == {'ok', "pong"} }))
    f = f + check("bad_agents_cost_one_result_each", oks == 2 && length(rs) == 4, rs)
    if (f == 0) { print("OK agent tests") ; sys_exit(0) } else { sys_exit(1) }
}
