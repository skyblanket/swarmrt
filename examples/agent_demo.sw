# agent_demo.sw — the canonical agent loop in ~30 lines using lib/Agent.sw.
#
# This is examples/llm_agent.sw rewritten on top of the Agent module: the
# whole receive -> stream -> parse-tools -> dispatch -> loop machinery is
# gone, replaced by Agent.new + Agent.ask. You bring an endpoint and a list
# of tools; the module runs the loop.
#
# Set up:
#   export API_KEY=sk-...                  # bearer token (required)
#   export API_URL=https://api.moonshot.ai/v1/chat/completions  # optional
#   export MODEL=kimi-k2.6                                       # optional
#
# Run:
#   ./bin/swc build examples/agent_demo.sw -o /tmp/agent_demo
#   /tmp/agent_demo
#   > what files are in /tmp?
#
# Tool handlers extract args defensively (nil-check, never `expect`) so a
# malformed tool call comes back to the model as a tool-error string it can
# correct — the agent never panics on bad LLM output.

module AgentDemo
export [main]

import Agent

# ---- tools: each is %{name, description, handler: fun(args) -> string} ----

fun bash_tool(args) {
    cmd = map_get(args, 'cmd')
    if (cmd == nil) { "error: bash requires 'cmd'" }
    else { r = shell(cmd) ; elem(r, 1) }
}

fun read_tool(args) {
    path = map_get(args, 'path')
    if (path == nil) { "error: read requires 'path'" }
    else {
        content = file_read(path)
        if (content == nil) { f"error: cannot read {path}" } else { content }
    }
}

fun build_agent(key, url, model) {
    Agent.new(%{
        url: url,
        key: key,
        model: model,
        system: "You are a helpful agent that uses tools to answer questions.",
        tools: [
            %{name: "bash", description: "run a shell command: {\"cmd\": \"ls /tmp\"}",
              handler: fun(args) { bash_tool(args) }},
            %{name: "read", description: "read a file: {\"path\": \"/etc/hosts\"}",
              handler: fun(args) { read_tool(args) }}
        ]
    })
}

fun main() {
    key = getenv("API_KEY")
    if (key == nil) {
        print("error: set API_KEY (any OpenAI-compatible bearer token)")
        sys_exit(1)
    }
    url = case getenv("API_URL") { nil -> "https://api.moonshot.ai/v1/chat/completions" ; u -> u }
    model = case getenv("MODEL") { nil -> "kimi-k2.6" ; m -> m }
    agent = build_agent(key, url, model)

    print(f"agent_demo — model={model}\nask anything (empty line to quit):\n")
    repl(agent)
}

fun repl(agent) {
    q = read_line("> ")
    if (q == nil || string_length(q) == 0) { 'bye' }
    else {
        case Agent.ask(agent, q) {
            {'ok', reply}  -> print(f"\n→ {reply}")
            {'error', why} -> print(f"\n[agent error: {why}]")
        }
        print("")
        repl(agent)
    }
}
