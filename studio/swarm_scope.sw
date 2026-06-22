module SwarmScope

# SwarmScope - a live visual stress lab for SwarmRT.
#
# Run from the repository root:
#   bin/swc build studio/swarm_scope.sw -o /tmp/swarm_scope
#   SW_MAX_PROCS=12000 /tmp/swarm_scope
#   open http://localhost:4010
#
# The workload is real: every pair is two SwarmRT processes continuously
# exchanging ping/pong messages. The dashboard samples the live process table
# and streams aggregate runtime telemetry plus a process sample over WebSocket.

let dashboard_path = "studio/swarm_scope.html"
let port = 4010
let default_pairs = 750
let sample_limit = 420

fun value_or(v, fallback) {
    if (v == nil) { fallback } else { v }
}

fun counter(stats, key) {
    value_or(ets_get(stats, key), 0)
}

# ---------------------------------------------------------------------------
# Real actor workload
# ---------------------------------------------------------------------------

fun responder(stats, id) {
    receive {
        {'ping', from, seq, sent_at} ->
            send(from, {'pong', seq, sent_at})
            responder(stats, id)

        'stop' ->
            ets_update_counter(stats, 'actors_stopped', 1, 0)
            'ok'
    }
}

fun driver(stats, id, peer, seq) {
    sent_at = timestamp()
    send(peer, {'ping', self(), seq, sent_at})
    receive {
        {'pong', seq2, started} ->
            ets_update_counter(stats, 'messages_total', 2, 0)
            ets_update_counter(stats, 'roundtrips_total', 1, 0)
            ets_update_counter(stats, 'latency_total_ms', timestamp() - started, 0)
            sleep(20 + (id % 17))
            driver(stats, id, peer, seq2 + 1)

        'stop' ->
            send(peer, 'stop')
            ets_update_counter(stats, 'actors_stopped', 1, 0)
            'ok'
    }
}

fun add_pairs(stats, pairs, current, target, next_id) {
    if (current >= target) {
        {pairs, current, next_id}
    } else {
        peer = spawn(responder(stats, next_id))
        lead = spawn(driver(stats, next_id, peer, 0))
        ets_update_counter(stats, 'actors_spawned', 2, 0)
        add_pairs(stats, [{lead, peer} | pairs], current + 1, target, next_id + 1)
    }
}

fun remove_pairs(stats, pairs, current, target) {
    if (current <= target || length(pairs) == 0) {
        {pairs, current}
    } else {
        pair = hd(pairs)
        send(elem(pair, 0), 'stop')
        remove_pairs(stats, tl(pairs), current - 1, target)
    }
}

fun resize_pairs(stats, pairs, current, target, next_id) {
    safe_target = if (target < 0) { 0 } else { target }
    if (safe_target > current) {
        add_pairs(stats, pairs, current, safe_target, next_id)
    } else {
        shrunk = remove_pairs(stats, pairs, current, safe_target)
        {elem(shrunk, 0), elem(shrunk, 1), next_id}
    }
}

fun burst_work(stats, n, acc) {
    if (n <= 0) {
        ets_update_counter(stats, 'burst_completed', 1, 0)
        acc
    } else {
        burst_work(stats, n - 1, acc + ((n * 17) % 97))
    }
}

fun spawn_burst(stats, count) {
    if (count <= 0) {
        'ok'
    } else {
        spawn(burst_work(stats, 180, 0))
        ets_update_counter(stats, 'actors_spawned', 1, 0)
        spawn_burst(stats, count - 1)
    }
}

fun controller(stats, pairs, pair_count, next_id) {
    receive {
        {'scale', target_pairs} ->
            resized = resize_pairs(stats, pairs, pair_count, target_pairs, next_id)
            ets_put(stats, 'workload_pairs', elem(resized, 1))
            controller(stats, elem(resized, 0), elem(resized, 1), elem(resized, 2))

        {'burst', count} ->
            spawn_burst(stats, count)
            controller(stats, pairs, pair_count, next_id)
    }
}

# ---------------------------------------------------------------------------
# Supervised crash laboratory
# ---------------------------------------------------------------------------

fun guard_worker(stats) {
    ets_update_counter(stats, 'guard_starts', 1, 0)
    receive {
        'boom' ->
            ets_update_counter(stats, 'crashes_total', 1, 0)
            panic("SwarmScope injected failure")

        {'ping', from} ->
            send(from, 'pong')
            guard_worker(stats)
    }
}

fun start_guards(stats, sup, n, names) {
    if (n <= 0) {
        names
    } else {
        name = "scope_guard_" ++ to_string(n)
        sup_start_child(sup, {name, fun() { guard_worker(stats) }, 'permanent'})
        start_guards(stats, sup, n - 1, [name | names])
    }
}

fun crash_guards(names) {
    if (length(names) == 0) {
        'ok'
    } else {
        pid = whereis(hd(names))
        if (pid != nil) { send(pid, 'boom') } else { 'missing' }
        crash_guards(tl(names))
    }
}

fun chaos_manager(stats, names) {
    receive {
        'chaos' ->
            ets_update_counter(stats, 'chaos_events', 1, 0)
            crash_guards(names)
            chaos_manager(stats, names)
    }
}

# ---------------------------------------------------------------------------
# Host process sampler
# ---------------------------------------------------------------------------

# The shell is a direct child of SwarmScope, so $PPID is the runtime process.
# Sampling every two seconds keeps this observability path off the hot loop.
fun system_sampler(stats) {
    result = shell("ps -o rss= -o %cpu= -p $PPID | xargs")
    if (result != nil && elem(result, 0) == 0) {
        parts = string_split(string_trim(elem(result, 1)), " ")
        if (length(parts) >= 2) {
            rss = to_int(hd(parts))
            cpu = to_float(hd(tl(parts)))
            if (rss != nil) { ets_put(stats, 'rss_kb', rss) } else { 'no_rss' }
            if (cpu != nil) { ets_put(stats, 'cpu_pct', cpu) } else { 'no_cpu' }
        } else {
            'bad_ps'
        }
    } else {
        'no_ps'
    }
    sleep(2000)
    system_sampler(stats)
}

# ---------------------------------------------------------------------------
# Runtime telemetry
# ---------------------------------------------------------------------------

fun status_add(status, running, runnable, waiting, exiting) {
    if (status == 'running') { {running + 1, runnable, waiting, exiting} }
    else { if (status == 'runnable') { {running, runnable + 1, waiting, exiting} }
    else { if (status == 'waiting') { {running, runnable, waiting + 1, exiting} }
    else { {running, runnable, waiting, exiting + 1} }}}
}

fun sample_row(info) {
    %{
        pid: map_get(info, 'pid'),
        status: map_get(info, 'status'),
        reductions: map_get(info, 'reductions'),
        messages: map_get(info, 'messages'),
        heap: map_get(info, 'heap_used'),
        name: value_or(map_get(info, 'name'), "")
    }
}

fun scan_processes(pids, running, runnable, waiting, exiting,
                   reductions, queued, heap, left, samples) {
    if (length(pids) == 0) {
        {running, runnable, waiting, exiting, reductions, queued, heap, samples}
    } else {
        info = process_info(hd(pids))
        if (info == nil) {
            scan_processes(tl(pids), running, runnable, waiting, exiting,
                           reductions, queued, heap, left, samples)
        } else {
            counts = status_add(map_get(info, 'status'), running, runnable, waiting, exiting)
            next_samples = if (left > 0) { [sample_row(info) | samples] } else { samples }
            next_left = if (left > 0) { left - 1 } else { 0 }
            scan_processes(
                tl(pids),
                elem(counts, 0), elem(counts, 1), elem(counts, 2), elem(counts, 3),
                reductions + value_or(map_get(info, 'reductions'), 0),
                queued + value_or(map_get(info, 'messages'), 0),
                heap + value_or(map_get(info, 'heap_used'), 0),
                next_left,
                next_samples
            )
        }
    }
}

fun make_snapshot(stats, last_messages, last_time) {
    now = timestamp()
    messages = counter(stats, 'messages_total')
    elapsed = now - last_time
    rate = if (elapsed <= 0) { 0 } else { ((messages - last_messages) * 1000) / elapsed }
    roundtrips = counter(stats, 'roundtrips_total')
    latency_total = counter(stats, 'latency_total_ms')
    avg_latency = if (roundtrips <= 0) { 0 } else { latency_total / roundtrips }

    pids = process_list()
    scan = scan_processes(pids, 0, 0, 0, 0, 0, 0, 0, sample_limit, [])
    snapshot = %{
        type: "snapshot",
        now: now,
        uptime_ms: now - counter(stats, 'boot_ms'),
        processes: length(pids),
        running: elem(scan, 0),
        runnable: elem(scan, 1),
        waiting: elem(scan, 2),
        exiting: elem(scan, 3),
        reductions: elem(scan, 4),
        queued: elem(scan, 5),
        heap_used: elem(scan, 6),
        samples: elem(scan, 7),
        message_rate: rate,
        messages_total: messages,
        roundtrips_total: roundtrips,
        avg_latency_ms: avg_latency,
        workload_pairs: counter(stats, 'workload_pairs'),
        actors_spawned: counter(stats, 'actors_spawned'),
        actors_stopped: counter(stats, 'actors_stopped'),
        burst_completed: counter(stats, 'burst_completed'),
        crashes_total: counter(stats, 'crashes_total'),
        guard_starts: counter(stats, 'guard_starts'),
        chaos_events: counter(stats, 'chaos_events'),
        registered: length(registered()),
        schedulers: value_or(getenv("SW_SCHEDULERS"), "auto"),
        rss_kb: counter(stats, 'rss_kb'),
        cpu_pct: counter(stats, 'cpu_pct')
    }
    {snapshot, messages, now}
}

fun handle_command(raw) {
    msg = json_decode(raw)
    if (msg == nil) {
        'bad_json'
    } else {
        action = map_get(msg, 'action')
        value = value_or(map_get(msg, 'value'), 0)
        if (action == "scale") {
            send(whereis("swarm_scope_controller"), {'scale', value})
        } else { if (action == "burst") {
            send(whereis("swarm_scope_controller"), {'burst', value})
        } else { if (action == "chaos") {
            send(whereis("swarm_scope_chaos"), 'chaos')
        } else {
            'unknown'
        }}}
    }
}

fun dashboard_session(ws, stats, last_messages, last_time) {
    snap = make_snapshot(stats, last_messages, last_time)
    ws_send(ws, json_encode(elem(snap, 0)))
    receive {
        {'ws_message', _conn, raw} ->
            handle_command(raw)
            dashboard_session(ws, stats, elem(snap, 1), elem(snap, 2))

        {'ws_close', _conn} ->
            'ok'

        after 500 {
            dashboard_session(ws, stats, elem(snap, 1), elem(snap, 2))
        }
    }
}

# ---------------------------------------------------------------------------
# HTTP / WebSocket server
# ---------------------------------------------------------------------------

fun serve(stats) {
    receive {
        {'http_request', conn, 'GET', "/", _headers, _body} ->
            html = file_read(dashboard_path)
            if (html == nil) {
                http_respond(conn, 500, "Content-Type: text/plain\r\n",
                             "Run SwarmScope from the swarmrt repository root.\n")
            } else {
                http_respond(conn, 200, "Content-Type: text/html\r\n", html)
            }
            serve(stats)

        {'http_request', conn, 'GET', "/favicon.ico", _headers, _body} ->
            http_respond(conn, 204, "", "")
            serve(stats)

        {'ws_connect', conn, "/scope/ws"} ->
            pid = spawn(dashboard_session(conn, stats, counter(stats, 'messages_total'), timestamp()))
            ws_set_handler(conn, pid)
            serve(stats)

        _other ->
            serve(stats)
    }
}

fun main() {
    stats = ets_new()
    ets_put(stats, 'boot_ms', timestamp())
    ets_put(stats, 'workload_pairs', 0)

    sup = dyn_supervisor(1000, 60)
    guards = start_guards(stats, sup, 16, [])
    chaos = spawn(chaos_manager(stats, guards))
    register('swarm_scope_chaos', chaos)

    control = spawn(controller(stats, [], 0, 1))
    register('swarm_scope_controller', control)
    spawn(system_sampler(stats))

    configured = to_int(getenv("SWARM_SCOPE_PAIRS"))
    initial_pairs = if (configured == nil) { default_pairs } else { configured }
    send(control, {'scale', initial_pairs})

    http_listen(port)
    print("")
    print("  SwarmScope is live")
    print("  http://localhost:" ++ to_string(port))
    print("  workload: " ++ to_string(initial_pairs * 2) ++ " ping-pong actors + supervised guards")
    print("")
    serve(stats)
}
