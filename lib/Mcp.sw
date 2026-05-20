# Mcp.sw — Model Context Protocol client + server, written in sw.
#
# MCP is JSON-RPC 2.0 over (typically) newline-delimited stdio. This
# module gives you the obvious sw API:
#
#   ## Client (talk to an MCP server)
#     c = Mcp.client_start("npx -y @modelcontextprotocol/server-filesystem /tmp")
#     tools = Mcp.list_tools(c)
#     result = Mcp.call_tool(c, "read_file", %{path: "/tmp/x"})
#     Mcp.close(c)
#
#   ## Server (expose sw tools as MCP over stdio)
#     Mcp.serve(%{
#         tools: [
#             %{name: "echo", description: "echo back",
#               input_schema: %{type: "object", properties: %{msg: %{type: "string"}}},
#               handler: fun(args) { map_get(args, 'msg') }}
#         ]
#     })
#
# Lookup falls back to <swarmrt-root>/lib/, so `import Mcp` works from
# any project without copying.

module Mcp

export [
    client_start, list_tools, call_tool, list_resources, read_resource, close,
    serve
]

# ============================================================
# CLIENT
# ============================================================

# client_start(cmd) → %{handle, next_id} | %{error: msg}
# Spawns the MCP server subprocess and does the `initialize` handshake.
fun client_start(cmd) {
    h = subprocess_spawn(cmd)
    if (h < 0) { %{error: f"failed to spawn '{cmd}'"} }
    else {
        client = %{handle: h, next_id: 1}
        # Initialize handshake — required before any other call.
        init_resp = rpc_call(client, "initialize", %{
            protocolVersion: "2024-11-05",
            capabilities: %{},
            clientInfo: %{name: "swarmrt", version: "0.1"}
        })
        if (map_get(init_resp, 'error') != nil) {
            subprocess_close(h)
            init_resp
        } else {
            # Send the `initialized` notification — no id, no response.
            send_notification(client, "notifications/initialized", %{})
            map_put(client, 'next_id', map_get(client, 'next_id') + 1)
        }
    }
}

fun list_tools(client) {
    resp = rpc_call(client, "tools/list", %{})
    case map_get(resp, 'error') {
        nil -> map_get(map_get(resp, 'result'), 'tools')
        e   -> %{error: e}
    }
}

fun call_tool(client, name, args) {
    resp = rpc_call(client, "tools/call", %{name: name, arguments: args})
    case map_get(resp, 'error') {
        nil -> map_get(resp, 'result')
        e   -> %{error: e}
    }
}

fun list_resources(client) {
    resp = rpc_call(client, "resources/list", %{})
    case map_get(resp, 'error') {
        nil -> map_get(map_get(resp, 'result'), 'resources')
        e   -> %{error: e}
    }
}

fun read_resource(client, uri) {
    resp = rpc_call(client, "resources/read", %{uri: uri})
    case map_get(resp, 'error') {
        nil -> map_get(resp, 'result')
        e   -> %{error: e}
    }
}

fun close(client) {
    subprocess_close(map_get(client, 'handle'))
}

# ---- Internal: JSON-RPC framing ----

fun rpc_call(client, method, params) {
    id = map_get(client, 'next_id')
    h  = map_get(client, 'handle')
    req = json_encode(%{
        jsonrpc: "2.0",
        id: id,
        method: method,
        params: params
    })
    subprocess_send_line(h, req)
    # Wait for the matching response. MCP servers may emit unsolicited
    # notifications (no id) before our reply lands — drain those.
    wait_for_id(h, id, 30000)
}

fun wait_for_id(h, want_id, timeout_ms) {
    line = subprocess_recv_line(h, timeout_ms)
    if (line == nil) { %{error: "timeout"} }
    else {
        msg = json_decode(line)
        case msg {
            nil -> %{error: f"unparseable response: {line}"}
            _   ->
                case map_get(msg, 'id') {
                    nil  -> wait_for_id(h, want_id, timeout_ms)   # notification — skip
                    rid when rid == want_id -> msg
                    _    -> wait_for_id(h, want_id, timeout_ms)   # different id — skip
                }
        }
    }
}

fun send_notification(client, method, params) {
    h = map_get(client, 'handle')
    msg = json_encode(%{jsonrpc: "2.0", method: method, params: params})
    subprocess_send_line(h, msg)
}

# ============================================================
# SERVER
# ============================================================
#
# Reads JSON-RPC requests from stdin, dispatches to tool handlers,
# writes responses to stdout. Blocks until stdin EOFs.
#
# config = %{
#     name: "my-mcp-server",
#     version: "0.1",
#     tools: [%{name, description, input_schema, handler}, ...]
# }

fun serve(config) {
    serve_loop(config)
}

fun serve_loop(config) {
    line = read_line(nil)
    case line {
        nil -> 'eof'
        _   ->
            req = json_decode(line)
            case req {
                nil -> serve_loop(config)  # bogus input, skip
                _   ->
                    method = to_string(map_get(req, 'method'))
                    id = map_get(req, 'id')
                    params = map_get(req, 'params')
                    handle_request(config, method, id, params)
                    serve_loop(config)
            }
    }
}

fun handle_request(config, method, id, params) {
    if (id == nil) {
        # Notification — no response.
        'noop'
    } else {
        result = dispatch(config, method, params)
        resp_body = case map_get(result, 'error') {
            nil -> %{jsonrpc: "2.0", id: id, result: result}
            e   -> %{jsonrpc: "2.0", id: id, error: %{code: -32603, message: to_string(e)}}
        }
        print(json_encode(resp_body))
    }
}

fun dispatch(config, method, params) {
    case method {
        "initialize" ->
            %{
                protocolVersion: "2024-11-05",
                capabilities: %{tools: %{}},
                serverInfo: %{
                    name: case map_get(config, 'name') { nil -> "swarmrt-mcp" ; n -> n },
                    version: case map_get(config, 'version') { nil -> "0.1" ; v -> v }
                }
            }
        "tools/list" ->
            %{tools: tools_summaries(map_get(config, 'tools'), [])}
        "tools/call" ->
            tname = to_string(map_get(params, 'name'))
            targs = map_get(params, 'arguments')
            run_tool(map_get(config, 'tools'), tname, targs)
        _ ->
            %{error: f"method not found: {method}"}
    }
}

fun tools_summaries(tools, acc) {
    if (tools == nil || length(tools) == 0) { acc }
    else {
        t = hd(tools)
        summary = %{
            name: to_string(map_get(t, 'name')),
            description: to_string(map_get(t, 'description')),
            inputSchema: map_get(t, 'input_schema')
        }
        tools_summaries(tl(tools), list_append(acc, summary))
    }
}

fun run_tool(tools, name, args) {
    if (tools == nil || length(tools) == 0) {
        %{error: f"no such tool: {name}"}
    } else {
        t = hd(tools)
        if (to_string(map_get(t, 'name')) == name) {
            handler = map_get(t, 'handler')
            result = try { handler(args) } catch e { f"tool error: {e}" }
            %{content: [%{type: "text", text: to_string(result)}]}
        } else {
            run_tool(tl(tools), name, args)
        }
    }
}
