module Test_agent

# Unit test for the lib/Agent.sw canonical agent loop, driven by a MOCK
# LLM (the chat_fn injection seam) so it runs offline. Covers:
#   1. the full tool loop: model emits a <tool> tag -> Agent dispatches the
#      handler -> appends the result -> model gets it and gives a final
#      plain-text answer. {'ok', reply} comes back.
#   2. tool-arg errors return a tool-error STRING, never panic (the bug the
#      hand-rolled example shipped). An unknown tool likewise.
#   3. a step-limit overrun returns {'error', _}, not a hang.
#   4. ask_json validate-and-retry: first reply is invalid JSON, retry
#      produces a valid object with the required keys -> {'ok', map}.
#   5. ask_json gives up with {'error', _} after retries are exhausted.

import Agent
import Std

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# A mock LLM: a closure over a shared ETS step counter that returns a
# scripted sequence of assistant turns. `script` is a list of contents to
# return in order; once exhausted it returns the last one forever.
fun mock_chat(script) {
    state = ets_new()
    ets_put(state, 'i', 0)
    fun(_history) {
        i = ets_get(state, 'i')
        ets_put(state, 'i', i + 1)
        content = if (i < length(script)) { _at(script, i) } else { _at(script, length(script) - 1) }
        {'ok', content}
    }
}

fun _at(lst, i) { if (i == 0) { hd(lst) } else { _at(tl(lst), i - 1) } }

# ---- 1. full tool loop ----
fun test_tool_loop() {
    # Turn 1: model calls the `echo` tool. Turn 2: final answer.
    chat = mock_chat([
        "Let me use a tool.\n<tool name=\"echo\">{\"msg\": \"hi\"}</tool>",
        "The tool said: ECHO:hi. Done."
    ])
    agent = Agent.new(%{
        url: "mock", key: "mock", model: "mock", chat_fn: chat,
        tools: [%{name: "echo", description: "echo a msg",
                  handler: fun(args) {
                      msg = map_get(args, 'msg')
                      if (msg == nil) { "error: echo requires 'msg'" } else { "ECHO:" ++ msg }
                  }}]
    })
    case Agent.ask(agent, "please echo hi") {
        {'ok', reply} -> assert_eq("tool_loop_final", string_contains(reply, "ECHO:hi"), 'true')
        {'error', why} -> assert_eq("tool_loop_final", "error:" ++ why, "ok")
    }
}

# ---- 2a. missing-arg handler returns a tool-error string, no panic ----
# The model calls echo with NO msg; the handler must return an error string
# that the model then sees. Turn 2 is the final answer. If Agent panicked on
# the missing arg this test would crash instead of completing.
fun test_tool_arg_error_no_panic() {
    chat = mock_chat([
        "<tool name=\"echo\">{}</tool>",
        "Got an error, here is my final answer."
    ])
    agent = Agent.new(%{
        url: "mock", key: "mock", model: "mock", chat_fn: chat,
        tools: [%{name: "echo", description: "echo",
                  handler: fun(args) {
                      msg = map_get(args, 'msg')
                      if (msg == nil) { "error: echo requires 'msg'" } else { "ECHO:" ++ msg }
                  }}]
    })
    case Agent.ask(agent, "echo") {
        {'ok', reply} -> assert_eq("arg_error_no_panic", string_contains(reply, "final answer"), 'true')
        {'error', _}  -> assert_eq("arg_error_no_panic", "error", "ok")
    }
}

# ---- 2b. unknown tool returns an error string ----
fun test_unknown_tool() {
    chat = mock_chat([
        "<tool name=\"nope\">{}</tool>",
        "ok done"
    ])
    agent = Agent.new(%{
        url: "mock", key: "mock", model: "mock", chat_fn: chat, tools: []
    })
    case Agent.ask(agent, "go") {
        {'ok', reply} -> assert_eq("unknown_tool", string_contains(reply, "done"), 'true')
        {'error', _}  -> assert_eq("unknown_tool", "error", "ok")
    }
}

# ---- 3. step-limit overrun -> {'error', _}, not a hang ----
# The model emits a tool tag forever; with max_steps 3 Agent must bail with
# an error rather than loop indefinitely.
fun test_step_limit() {
    chat = mock_chat(["<tool name=\"echo\">{\"msg\":\"x\"}</tool>"])
    agent = Agent.new(%{
        url: "mock", key: "mock", model: "mock", chat_fn: chat, max_steps: 3,
        tools: [%{name: "echo", description: "echo",
                  handler: fun(args) { "ECHO" }}]
    })
    case Agent.ask(agent, "loop forever") {
        {'error', why} -> assert_eq("step_limit", string_contains(why, "step limit"), 'true')
        {'ok', _}      -> assert_eq("step_limit", "ok-unexpected", "error")
    }
}

# ---- 4. ask_json validate + retry ----
# Turn 1: invalid JSON. Turn 2: valid object with required keys. Agent
# re-asks on the validation failure and succeeds on the retry.
fun test_ask_json_retry() {
    chat = mock_chat([
        "here is your answer: not actually json",
        "{\"name\": \"Ada\", \"score\": 9}"
    ])
    agent = Agent.new(%{url: "mock", key: "mock", model: "mock", chat_fn: chat})
    case Agent.ask_json(agent, "give me name+score", %{required: ['name', 'score'], retries: 2}) {
        {'ok', m}      -> assert_eq("ask_json_retry", map_get(m, 'name'), "Ada")
        {'error', why} -> assert_eq("ask_json_retry", "error:" ++ why, "ok")
    }
}

# ---- 5. ask_json exhausts retries -> {'error', _} ----
fun test_ask_json_giveup() {
    chat = mock_chat(["never valid json"])
    agent = Agent.new(%{url: "mock", key: "mock", model: "mock", chat_fn: chat})
    case Agent.ask_json(agent, "x", %{required: ['a'], retries: 1}) {
        {'error', _} -> assert_eq("ask_json_giveup", 'true', 'true')
        {'ok', _}    -> assert_eq("ask_json_giveup", "ok-unexpected", "error")
    }
}

# ---- 6. ask_json strips a ```json code fence before validating ----
fun test_ask_json_fence() {
    chat = mock_chat(["```json\n{\"a\": 1}\n```"])
    agent = Agent.new(%{url: "mock", key: "mock", model: "mock", chat_fn: chat})
    case Agent.ask_json(agent, "x", %{required: ['a'], retries: 0}) {
        {'ok', m}    -> assert_eq("ask_json_fence", map_get(m, 'a'), 1)
        {'error', _} -> assert_eq("ask_json_fence", "error", "ok")
    }
}

fun main() {
    fails = 0
    fails = fails + test_tool_loop()
    fails = fails + test_tool_arg_error_no_panic()
    fails = fails + test_unknown_tool()
    fails = fails + test_step_limit()
    fails = fails + test_ask_json_retry()
    fails = fails + test_ask_json_giveup()
    fails = fails + test_ask_json_fence()
    if (fails == 0) { print("OK agent 7/7") ; sys_exit(0) }
    else { print("FAIL agent " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
