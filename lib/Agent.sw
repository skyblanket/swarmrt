# Agent.sw — the canonical LLM agent loop, written in sw.
#
# This is the module that kills the most-copied boilerplate in every
# swarmrt agent: receive -> stream -> parse-tools -> case-dispatch -> loop.
# It is a THIN module, not a framework. You bring an LLM endpoint and a
# list of tools; it runs the full prompt/tool/loop cycle for you.
#
#   import Agent
#
#   agent = Agent.new(%{
#       url:    "https://api.moonshot.ai/v1/chat/completions",
#       key:    getenv("API_KEY"),
#       model:  "kimi-k2.6",
#       system: "You are a helpful filesystem assistant.",
#       tools:  [
#           %{name: "read", description: "read a file: {\"path\":\"/etc/hosts\"}",
#             handler: fun(args) {
#                 case map_get(args, 'path') {
#                     nil -> "error: read requires 'path'"
#                     p   -> case file_read(p) { nil -> f"error: cannot read {p}" ; c -> c }
#                 }
#             }}
#       ]
#   })
#
#   case Agent.ask(agent, "what's in /etc/hosts?") {
#       {'ok', reply}  -> print(reply)
#       {'error', why} -> print("agent failed:", why)
#   }
#
# The tool protocol is the inband-tag convention (the swarm-code / Gemma
# default): the model emits `<tool name="read">{"path":"/x"}</tool>` in its
# prose, Agent parses every such tag, dispatches to the matching handler,
# appends the results, and re-asks — until the model stops emitting tools
# and just answers, or the step limit is hit.
#
# Auto-resolves from <swarmrt-root>/lib/, so `import Agent` works anywhere.

module Agent

export [new, ask, ask_json, step_limit]

import Std

# Default cap on tool-call rounds. Override per-agent with %{max_steps: N}.
fun step_limit() { 8 }

# ============================================================
# CONSTRUCTION
# ============================================================

# new(opts) -> agent map. opts:
#   url     : OpenAI-compatible /chat/completions endpoint (required)
#   key     : bearer token (required)
#   model   : model id (required)
#   system  : extra system-prompt text (optional; the tool catalogue is
#             appended automatically so the model knows the tag protocol)
#   tools   : [%{name, description, handler: fun(args){...}}, ...] (optional)
#   max_steps : tool-call rounds before giving up (optional, default 8)
#   chat_fn : optional fun(history) -> {'ok', content} | {'error', why}.
#             When present it REPLACES the HTTP call — the injection seam
#             for tests (a mock LLM) and for non-OpenAI backends. When
#             absent, Agent uses the tagged http_post_stream.
fun new(opts) {
    tools = case map_get(opts, 'tools') { nil -> [] ; t -> t }
    max_steps = case map_get(opts, 'max_steps') { nil -> step_limit() ; m -> m }
    # Index tools by name for O(1) case-free dispatch, and pre-render the
    # tool catalogue once so every turn's system prompt is cheap.
    tool_map = _index_tools(tools, %{})
    catalogue = _render_catalogue(tools)
    user_system = case map_get(opts, 'system') { nil -> "" ; s -> s }
    %{
        url: map_get(opts, 'url'),
        key: map_get(opts, 'key'),
        model: map_get(opts, 'model'),
        tool_map: tool_map,
        max_steps: max_steps,
        chat_fn: map_get(opts, 'chat_fn'),
        system: _full_system(user_system, catalogue)
    }
}

fun _index_tools(tools, acc) {
    if (length(tools) == 0) { acc }
    else {
        t = hd(tools)
        name = map_get(t, 'name')
        _index_tools(tl(tools), map_put(acc, name, t))
    }
}

# The system prompt the model actually sees: the caller's text + the tool
# protocol + the tool catalogue. If there are no tools we skip the protocol
# section entirely (a plain chat agent).
fun _full_system(user_system, catalogue) {
    if (catalogue == "") { user_system }
    else {
        proto = "\n\nYou can call tools by emitting this tag verbatim in your reply:\n" ++
                "  <tool name=\"TOOLNAME\">{\"arg\": \"value\"}</tool>\n" ++
                "The runtime executes it and appends the result, then you continue. " ++
                "When you have the final answer, reply in plain text with NO tool tags.\n\n" ++
                "Available tools:\n" ++ catalogue
        user_system ++ proto
    }
}

fun _render_catalogue(tools) {
    if (length(tools) == 0) { "" }
    else {
        t = hd(tools)
        desc = case map_get(t, 'description') { nil -> "" ; d -> d }
        line = "  - " ++ map_get(t, 'name') ++ ": " ++ desc ++ "\n"
        line ++ _render_catalogue(tl(tools))
    }
}

# ============================================================
# ASK — the full loop
# ============================================================

# ask(agent, user_msg) -> {'ok', reply} | {'error', reason}
# Runs the prompt -> tool -> loop cycle and returns the model's final
# plain-text answer, or an {'error', _} if the LLM call failed or the step
# limit was hit before the model produced a tool-free answer.
fun ask(agent, user_msg) {
    history = [%{role: "system", content: map_get(agent, 'system')},
               %{role: "user", content: user_msg}]
    _loop(agent, history, 0)
}

fun _loop(agent, history, step) {
    if (step >= map_get(agent, 'max_steps')) {
        {'error', "step limit reached without a final answer"}
    } else {
        case _chat(agent, history) {
            {'error', why} -> ({'error', why})
            {'ok', content} ->
                calls = _parse_tools(content)
                if (length(calls) == 0) {
                    # No tool tags -> this is the final answer.
                    {'ok', content}
                } else {
                    results = _run_calls(agent, calls, [])
                    with_a = list_append(history, %{role: "assistant", content: content})
                    with_r = list_append(with_a, %{role: "user", content: _format_results(results)})
                    _loop(agent, with_r, step + 1)
                }
        }
    }
}

# ============================================================
# TOOL DISPATCH — never panics
# ============================================================

# Run every parsed call against the agent's tool map. A missing tool or a
# bad-args handler returns a tool-error STRING that goes back to the model
# (which can then correct itself) — we never `expect`/panic on tool input,
# the crash the hand-rolled example shipped with. (A handler that itself
# calls panic() is the handler author's bug; use error()/return-strings.)
fun _run_calls(agent, calls, acc) {
    if (length(calls) == 0) { acc }
    else {
        c = hd(calls)
        name = elem(c, 0)
        args = elem(c, 1)
        result = _dispatch(agent, name, args)
        _run_calls(agent, tl(calls), list_append(acc, {name, result}))
    }
}

fun _dispatch(agent, name, args) {
    case map_get(map_get(agent, 'tool_map'), name) {
        nil -> f"error: unknown tool '{name}'"
        tool ->
            handler = map_get(tool, 'handler')
            if (handler == nil) { f"error: tool '{name}' has no handler" }
            else {
                # try/catch guards error()-style raises in the handler so one
                # bad call surfaces as a tool error, not an agent crash.
                safe_args = case args { nil -> %{} ; a -> a }
                try { to_string(handler(safe_args)) }
                catch e { f"error: tool '{name}' failed: {e}" }
            }
    }
}

fun _format_results(results) {
    if (length(results) == 0) { "" }
    else {
        r = hd(results)
        block = "<tool_result name=\"" ++ elem(r, 0) ++ "\">\n" ++ elem(r, 1) ++ "\n</tool_result>\n"
        block ++ _format_results(tl(results))
    }
}

# ============================================================
# STRUCTURED OUTPUT — ask + validate + retry
# ============================================================

# ask_json(agent, user_msg, opts) -> {'ok', map} | {'error', reason}
# The instructor-style "schema -> validate -> retry" loop in pure sw: ask
# the model, decode + check required keys via Std.json_expect, and on
# failure re-ask with the validation error appended so the model can fix
# its output. Retries up to opts.retries times (default 2).
#   opts: required : [keys...]   (passed through to Std.json_expect)
#         retries  : int         (default 2)
fun ask_json(agent, user_msg, opts) {
    retries = case map_get(opts, 'retries') { nil -> 2 ; r -> r }
    _ask_json_loop(agent, user_msg, opts, retries)
}

fun _ask_json_loop(agent, user_msg, opts, tries_left) {
    case ask(agent, user_msg) {
        {'error', why} -> ({'error', why})
        {'ok', reply} ->
            case Std.json_expect(_strip_fence(reply), opts) {
                {'ok', m} -> ({'ok', m})
                {'error', why} ->
                    if (tries_left <= 0) { {'error', f"validation failed after retries: {why}"} }
                    else {
                        retry_msg = user_msg ++
                            f"\n\nYour previous reply was not valid: {why}. " ++
                            "Reply with ONLY a single JSON object, no prose, no code fence."
                        _ask_json_loop(agent, retry_msg, opts, tries_left - 1)
                    }
            }
    }
}

# Strip a leading ```json / ``` fence if the model wrapped its JSON, so
# json_expect sees raw JSON. Best-effort: returns the input unchanged if
# there's no fence.
fun _strip_fence(s) {
    t = string_trim(s)
    if (string_starts_with(t, "```") == 'true') {
        # drop the first line (``` or ```json) and a trailing ```
        nl = string_index_of(t, "\n")
        if (nl < 0) { t }
        else {
            body = string_sub(t, nl + 1, string_length(t) - (nl + 1))
            close = string_index_of(body, "```")
            if (close < 0) { string_trim(body) }
            else { string_trim(string_sub(body, 0, close)) }
        }
    } else { t }
}

# ============================================================
# LLM CALL — tagged streaming chat completions
# ============================================================

# Returns {'ok', content_string} | {'error', reason}. Built on the TAGGED
# http_post_stream so a real failure (curl died / non-2xx / unparseable
# stream) comes back as {'error', why} the loop can surface, distinct from
# the model genuinely producing empty content ({'ok', ""}).
fun _chat(agent, history) {
    case map_get(agent, 'chat_fn') {
        nil -> _chat_http(agent, history)
        f   -> f(history)
    }
}

fun _chat_http(agent, history) {
    body = json_encode(%{
        model: map_get(agent, 'model'),
        messages: history,
        stream: 'true',
        max_tokens: 2048
    })
    hdrs = [{"Authorization", "Bearer " ++ map_get(agent, 'key')},
            {"Content-Type", "application/json"}]
    case http_post_stream(map_get(agent, 'url'), hdrs, body) {
        {'error', why} -> ({'error', why})
        {'ok', json} ->
            decoded = json_decode(json)
            if (decoded == nil) { {'error', "could not decode LLM response"} }
            else {
                choices = map_get(decoded, 'choices')
                if (choices == nil || length(choices) == 0) { {'ok', ""} }
                else {
                    msg = map_get(hd(choices), 'message')
                    content = case map_get(msg, 'content') { nil -> "" ; c -> c }
                    {'ok', content}
                }
            }
    }
}

# ============================================================
# TOOL-TAG PARSER — <tool name="X">{json}</tool> -> [{X, args_map}, ...]
# ============================================================

fun _parse_tools(text) { _parse_loop(text, []) }

fun _parse_loop(text, acc) {
    i = string_index_of(text, "<tool name=\"")
    if (i < 0) { acc }
    else {
        rest = string_sub(text, i + 12, string_length(text) - (i + 12))
        name_end = string_index_of(rest, "\"")
        if (name_end < 0) { acc }
        else {
            name = string_sub(rest, 0, name_end)
            after_name = string_sub(rest, name_end + 2, string_length(rest) - (name_end + 2))
            close = string_index_of(after_name, "</tool>")
            if (close < 0) { acc }
            else {
                args_str = string_sub(after_name, 0, close)
                args = json_decode(args_str)
                tail_text = string_sub(after_name, close + 7, string_length(after_name) - (close + 7))
                _parse_loop(tail_text, list_append(acc, {name, args}))
            }
        }
    }
}
