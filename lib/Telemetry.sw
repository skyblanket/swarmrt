# Telemetry.sw — observability primitive for agents.
#
# A single emit point with pluggable sinks. Common pattern:
#
#   Telemetry.configure(%{sinks: [Telemetry.stdout_sink(),
#                                  Telemetry.file_sink("/tmp/events.jsonl")]})
#
#   Telemetry.emit("tool.bash.start", %{cmd: "ls"})
#   ... run tool ...
#   Telemetry.emit("tool.bash.end", %{exit_code: 0, ms: 42})
#
# Events are %{name, ts_ms, ...attrs} maps. Sinks are functions taking
# the event map and doing whatever — write to file, push to OTel, emit
# to a Slack webhook, ets_put into a ring buffer for the dashboard.
#
# Implementation: a registered process (`telemetry_hub`) holds the
# sink list. emit() sends an `{'event', map}` message; the hub fans
# out to every sink.

module Telemetry
export [configure, emit, stdout_sink, file_sink, jsonl_sink, count, names]

# Public — start the hub, install sinks.
fun configure(opts) {
    sinks = case map_get(opts, 'sinks') { nil -> [] ; s -> s }
    existing = whereis('telemetry_hub')
    if (existing != nil) { send(existing, 'stop') ; sleep(20) }
    pid = spawn(hub_loop(sinks, 0))
    register('telemetry_hub', pid)
    'ok'
}

# Public — emit an event by name, optional attrs map.
fun emit(name, attrs) {
    enriched = enrich(name, attrs)
    hub = whereis('telemetry_hub')
    if (hub == nil) {
        # No hub configured — default to stdout so events aren't lost.
        stdout_sink_fn(enriched)
    } else {
        send(hub, {'event', enriched})
    }
}

# Public — diagnostic helpers.
fun count() {
    hub = whereis('telemetry_hub')
    if (hub == nil) { 0 }
    else {
        send(hub, {'count', self()})
        receive { {'count', n} -> n }
    }
}

fun names() {
    hub = whereis('telemetry_hub')
    if (hub == nil) { [] }
    else {
        send(hub, {'names', self()})
        receive { {'names', xs} -> xs }
    }
}

# ---- sinks ----

fun stdout_sink() { stdout_sink_fn }
fun file_sink(path) {
    fun(event) { file_append_event(path, event) }
}
fun jsonl_sink(path) { file_sink(path) }   # alias — file_sink already writes JSON-per-line

fun stdout_sink_fn(event) {
    print(json_encode(event))
}

fun file_append_event(path, event) {
    line = json_encode(event) ++ "\n"
    # file_write overwrites; we want append. Read-then-write is OK at
    # low event volumes; for high volume add a real file_append builtin.
    existing = case file_read(path) { nil -> "" ; s -> s }
    file_write(path, existing ++ line)
}

# ---- internal hub ----

fun hub_loop(sinks, total) {
    receive {
        {'event', event} ->
            fan_out(sinks, event)
            hub_loop(sinks, total + 1)
        {'count', from} ->
            send(from, {'count', total})
            hub_loop(sinks, total)
        {'names', from} ->
            send(from, {'names', []})   # name registry is a TODO
            hub_loop(sinks, total)
        'stop' -> 'done'
    }
}

fun fan_out(sinks, event) {
    if (length(sinks) == 0) { 'ok' }
    else {
        sink = hd(sinks)
        try { sink(event) } catch e { 'sink_failed' }
        fan_out(tl(sinks), event)
    }
}

fun enrich(name, attrs) {
    base = %{name: name, ts_ms: timestamp()}
    case attrs {
        nil -> base
        _   -> map_merge(base, attrs)
    }
}
