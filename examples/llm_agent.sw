# llm_agent.sw — a working LLM-driven agent in ~90 lines of sw.
#
# Demonstrates the agent-loop shape: prompt → http_post_stream → parse
# tool calls → case dispatch → loop. Backed by any OpenAI-compatible
# /v1/chat/completions endpoint.
#
# Set up:
#   export API_KEY=sk-...                  # bearer token (required)
#   export API_URL=https://api.moonshot.ai/v1/chat/completions  # optional
#   export MODEL=kimi-k2.6                                       # optional
#
# Run:
#   ./bin/swc build examples/llm_agent.sw -o /tmp/agent
#   /tmp/agent
#   > what files are in /tmp?
#
# The agent emits tool calls as `<tool name="bash">{"cmd":"ls /tmp"}</tool>`
# tags. Loop parses them, executes via the `case` dispatch below,
# appends results to history, and asks the model again. When the model
# stops emitting tool calls and just writes prose, that's the final
# answer.

module LlmAgent
export [main]

fun system_prompt() {
    "You are a helpful agent that uses tools to answer questions.\n\n" ++
    "Available tools:\n" ++
    "  <tool name=\"bash\">{\"cmd\": \"ls /tmp\"}</tool>      — run a shell command\n" ++
    "  <tool name=\"read\">{\"path\": \"/etc/hosts\"}</tool>  — read a file\n\n" ++
    "Call tools by emitting the <tool>…</tool> tag verbatim. The runtime " ++
    "will execute and append the result. When you have the answer, give " ++
    "a concise plain-text reply with no tools."
}

# ---- tool implementations: case-dispatched on the name ----

fun run_tool(name, args) {
    case name {
        "bash" ->
            cmd = expect(map_get(args, 'cmd'), "bash tool requires 'cmd'")
            r = shell(cmd)
            elem(r, 1)
        "read" ->
            path = expect(map_get(args, 'path'), "read tool requires 'path'")
            content = file_read(path)
            case content { nil -> f"error: cannot read {path}" ; c -> c }
        _ ->
            f"error: unknown tool '{name}'"
    }
}

# ---- the agent loop ----

fun loop(history, opts, step) {
    if (step >= 8) { print("\n[hit step limit]") ; 'done' }
    else {
        content = chat(history, opts)
        if (content == nil) { print("\n[llm error]") ; 'done' }
        else {
            calls = parse_tools(content)
            if (length(calls) == 0) {
                # No tool calls → final answer.
                print(f"\n→ {content}")
                'done'
            } else {
                results = run_each(calls, [])
                with_a = list_append(history, %{role: "assistant", content: content})
                with_r = list_append(with_a, %{role: "user", content: format_results(results)})
                loop(with_r, opts, step + 1)
            }
        }
    }
}

fun run_each(calls, acc) {
    if (length(calls) == 0) { acc }
    else {
        c = hd(calls)
        name = elem(c, 0)
        args = elem(c, 1)
        print(f"  ⏺ tool: {name}({json_encode(args)})")
        result = try { run_tool(name, args) } catch e { f"error: {e}" }
        run_each(tl(calls), list_append(acc, {name, result}))
    }
}

fun format_results(results) {
    if (length(results) == 0) { "" }
    else {
        r = hd(results)
        block = f"<tool_result name=\"{elem(r, 0)}\">\n{elem(r, 1)}\n</tool_result>\n"
        block ++ format_results(tl(results))
    }
}

# ---- LLM call (OpenAI-compatible streaming chat completions) ----

fun chat(history, opts) {
    body = json_encode(%{
        model: map_get(opts, 'model'),
        messages: history,
        stream: 'true',
        max_tokens: 2048
    })
    hdrs = [{"Authorization", "Bearer " ++ map_get(opts, 'key')},
            {"Content-Type", "application/json"}]
    resp = http_post_stream(map_get(opts, 'url'), hdrs, body)
    if (resp == nil) { nil }
    else {
        decoded = json_decode(resp)
        choices = map_get(decoded, 'choices')
        if (length(choices) == 0) { nil }
        else { map_get(map_get(hd(choices), 'message'), 'content') }
    }
}

# ---- tool-tag parser ----
# Matches <tool name="X">{json}</tool> and returns [{X, decoded_json}, ...].

fun parse_tools(text) { parse_loop(text, []) }

fun parse_loop(text, acc) {
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
                parse_loop(tail_text, list_append(acc, {name, args}))
            }
        }
    }
}

# ---- main ----

fun main() {
    key = getenv("API_KEY")
    if (key == nil) {
        print("error: set API_KEY (any OpenAI-compatible bearer token)")
        sys_exit(1)
    }
    url = case getenv("API_URL") { nil -> "https://api.moonshot.ai/v1/chat/completions" ; u -> u }
    model = case getenv("MODEL") { nil -> "kimi-k2.6" ; m -> m }
    opts = %{key: key, url: url, model: model}

    print(f"llm_agent — model={model}\nask anything (empty line to quit):\n")
    repl(opts)
}

fun repl(opts) {
    q = read_line("> ")
    if (q == nil || string_length(q) == 0) { 'bye' }
    else {
        history = [%{role: "system", content: system_prompt()},
                   %{role: "user", content: q}]
        loop(history, opts, 0)
        print("")
        repl(opts)
    }
}
