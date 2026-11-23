# Rollout.sw — run eval / RL rollout episodes against model APIs: one isolated
# process per episode, a deadline on each, typed failures that never fail the
# run, one trajectory JSONL row per episode, and resumable runs.
#
#   import Rollout
#   summary = Rollout.run(tasks, env, %{
#       max_concurrency: 64, episode_timeout_ms: 60000,
#       samples_per_task: 4, seed: 1, out: "rollouts.jsonl", resume: 'true',
#       llm: %{url: "http://127.0.0.1:8000/v1/chat/completions", model: "m"}})
#
# The env contract (docs/ROLLOUT.md has the full reference) is a map of
# functions, either single-shot:
#
#   %{run: fn(task, ctx) -> {reward, info}}
#
# or multi-turn, driven by a policy (usually the model):
#
#   %{reset:  fn(task, ctx) -> {state, obs},
#     policy: fn(obs, ctx) -> action,
#     step:   fn(state, action, ctx) -> {state, obs, reward, done},
#     score:  fn(state, ctx) -> reward}          # optional: else the step sum
#
# ctx is plain data: %{task, task_id, sample, seed, deadline, ...}. Inside env
# code, call the model with Rollout.llm(ctx, prompt_or_messages) and run tools
# with Rollout.tool(ctx, cmd, args, opts): both are recorded in the episode's
# trajectory and turn their failures into typed episode failures.
#
# Episode outcomes (the row's "kind"; "status" is "ok" only for "done"):
#   done          the env finished; "reward" holds its reward
#   timeout       the episode ran past episode_timeout_ms (killed)
#   step_timeout  one env call (reset / step / score) ran past step_timeout_ms
#   tool_timeout  a Rollout.tool call ran past its timeout (exec_managed's 124)
#   model_error   Rollout.llm failed after its retries; "error_kind" says how
#                 (rate_limited, server, timeout, transport, bad_request, auth,
#                 bad_response, config)
#   env_crash     env code raised or panicked
#   infra         the episode process died for another reason (killed, OOM cap)
# A failed episode's "reward" is null, never 0: an infra failure is not a
# zero-reward sample.
#
# Rows are written by the coordinator through task_stream's on_result as each
# episode ends, so a run killed with SIGKILL loses only the episodes in flight.
# With resume: 'true', episodes already in `out` (by task_id + sample, any
# outcome) are skipped.

module Rollout

import Std

export [run, llm, llm_with, request, tool, fail, load_done, episode_key]

# run(tasks, env, opts) → %{episodes, skipped, ran, ok, failed, kinds, wall_ms}
# kinds is %{done: n, timeout: n, ...} over the episodes this call ran.
fun run(tasks, env, opts) {
    t0 = timestamp()
    out = map_get(opts, 'out')
    resume = map_get(opts, 'resume') == 'true'
    if (out != nil && resume == 'false' && file_exists(out) == 'true') {
        panic(f"Rollout.run: {out} already exists; pass resume: 'true' to continue it, or remove it")
    }
    _check_env(env)
    samples = _opt(opts, 'samples_per_task', 1)
    base_seed = _opt(opts, 'seed', 0)
    llm_opts = _opt(opts, 'llm', %{})
    limit = map_get(opts, 'limit')
    if (limit != nil) { _apply_limit(llm_opts, limit) }

    all = _episodes(tasks, 0, samples, base_seed, [])
    done = if (resume == 'true' && out != nil) { load_done(out) } else { %{} }
    todo = _number(filter(all, fn(e) {
        map_has_key(done, episode_key(map_get(e, 'task_id'), map_get(e, 'sample'))) == 'false'
    }), 0, [])

    table = ets_new()
    ep_tmo = _opt(opts, 'episode_timeout_ms', 0)
    base_ctx = %{table: table, llm: llm_opts,
                 step_timeout_ms: _opt(opts, 'step_timeout_ms', 0),
                 tool_timeout_ms: _opt(opts, 'tool_timeout_ms', 30000),
                 max_steps: _opt(opts, 'max_steps', 100),
                 episode_timeout_ms: ep_tmo}
    ets_put(table, "kinds", %{})
    user_cb = map_get(opts, 'on_episode')
    Std.task_stream_each(todo, fn(ep) { _episode(env, ep, base_ctx) },
        %{max_concurrency: _opt(opts, 'max_concurrency', 16), timeout_ms: ep_tmo,
         on_result: fn(_i, ep, cell) {
             row = _row(table, ep, cell)
             if (out != nil) { file_append(out, json_encode(row) ++ "\n") }
             ets_put(table, "kinds", _bump(ets_get(table, "kinds"), map_get(row, 'kind')))
             if (user_cb != nil) { user_cb(row) }
         }})
    kinds = ets_get(table, "kinds")
    ok = _opt(kinds, 'done', 0)
    ran = length(todo)
    %{episodes: length(all), skipped: length(all) - ran, ran: ran, ok: ok,
      failed: ran - ok, kinds: kinds, wall_ms: timestamp() - t0}
}

# Rollout.llm(ctx, prompt_or_messages) → the completion text. Merges ctx's llm
# opts (run's opts.llm) with `extra` and bounds the call by the episode
# deadline. A failure after llm_request's retries ends the episode as
# model_error(kind). Recorded in the trajectory either way.
fun llm(ctx, prompt) { llm_with(ctx, prompt, %{}) }

fun llm_with(ctx, prompt, extra) {
    case request(ctx, prompt, extra) {
        {'ok', m} -> map_get(m, 'content')
        {'error', m} ->
            fail('model_error', f"{map_get(m, 'kind')}: {_clip(map_get(m, 'body'), 300)}",
                 map_get(m, 'kind'))
    }
}

# Rollout.request(ctx, prompt, extra) → llm_request's tagged result, never
# raising: for envs that handle model errors themselves. Recorded.
fun request(ctx, prompt, extra) {
    o = map_merge(map_get(ctx, 'llm'), extra)
    left = _left_ms(ctx)
    o = if (left > 0 && (map_get(o, 'timeout_ms') == nil || map_get(o, 'timeout_ms') > left)) {
        map_put(o, 'timeout_ms', left)
    } else { o }
    r = llm_request(prompt, o)
    ev = case r {
        {'ok', m} -> %{messages: _messages(prompt), output: map_get(m, 'content'),
                       finish_reason: map_get(m, 'finish_reason'), usage: map_get(m, 'usage'),
                       latency_ms: map_get(m, 'latency_ms'), attempts: map_get(m, 'attempts'),
                       error: nil}
        {'error', m} -> %{messages: _messages(prompt), output: nil, finish_reason: nil, usage: nil,
                          latency_ms: nil, attempts: map_get(m, 'attempts'),
                          error: %{kind: map_get(m, 'kind'), status: map_get(m, 'status'),
                                   retry_after_ms: map_get(m, 'retry_after_ms')}}
    }
    _push(ctx, "model", ev)
    r
}

# Rollout.tool(ctx, cmd, args, opts) → exec_managed's {exit_code, stdout}.
# opts are exec_managed's; timeout_ms defaults to run's tool_timeout_ms and is
# capped by the episode deadline. A timeout (exit 124) ends the episode as
# tool_timeout, after exec_managed has killed the tool's whole process group.
fun tool(ctx, cmd, args, opts) {
    tmo = _opt(opts, 'timeout_ms', map_get(ctx, 'tool_timeout_ms'))
    left = _left_ms(ctx)
    tmo = if (left > 0 && (tmo <= 0 || tmo > left)) { left } else { tmo }
    t0 = timestamp()
    r = exec_managed(cmd, args, map_put(opts, 'timeout_ms', tmo))
    ms = timestamp() - t0
    timed_out = elem(r, 0) == 124 && tmo > 0 && ms >= tmo - 50
    _push(ctx, "tool", %{cmd: cmd, args: args, exit_code: elem(r, 0), ms: ms,
                         timed_out: timed_out, output: _clip(elem(r, 1), 4000)})
    if (timed_out) { fail('tool_timeout', f"{cmd} ran past {tmo} ms", nil) }
    r
}

# Rollout.fail(kind, message, detail) — end the episode with a typed failure
# from env code (kind is one of the outcome atoms above; detail may be nil).
fun fail(kind, message, detail) { error({'rollout', kind, message, detail}) }

# The episodes already recorded in a JSONL file: %{"task_id#sample" => 'true'}.
# Unparseable lines (a write cut short by SIGKILL) are skipped.
fun load_done(path) {
    if (file_exists(path) == 'false') { %{} }
    else { _done_lines(string_split(file_read(path), "\n"), %{}) }
}

fun episode_key(task_id, sample) { f"{task_id}#{sample}" }

# ------------------------------------------------------------------ internals

fun _opt(m, k, dflt) {
    v = if (m == nil) { nil } else { map_get(m, k) }
    if (v == nil) { dflt } else { v }
}

fun _check_env(env) {
    has_run = map_get(env, 'run') != nil
    has_steps = map_get(env, 'reset') != nil && map_get(env, 'step') != nil && map_get(env, 'policy') != nil
    if (has_run == 'false' && has_steps == 'false') {
        panic("Rollout.run: env needs run: fn(task, ctx), or reset, policy and step")
    }
}

fun _apply_limit(llm_opts, limit) {
    key = _opt(llm_opts, 'url', _opt(llm_opts, 'provider', getenv("LLM_URL")))
    if (key == nil || key == "") { panic("Rollout.run: opts.limit needs opts.llm.url or opts.llm.provider") }
    if (llm_limit(key, limit) != 'ok') { panic(f"Rollout.run: llm_limit rejected {to_string(limit)}") }
}

# One descriptor per (task, sample), in task order then sample order.
fun _episodes(tasks, i, samples, base_seed, acc) {
    if (length(tasks) == 0) { acc }
    else {
        task = hd(tasks)
        tid = _opt(task, 'id', i)
        eps = Std.map(Std.range(0, samples), fn(s) {
            %{task: task, task_id: tid, sample: s, seed: base_seed + s}
        })
        _episodes(tl(tasks), i + 1, samples, base_seed, acc ++ eps)
    }
}

# Give each episode descriptor its slot (its index in this run's todo list).
fun _number(eps, i, acc) {
    if (length(eps) == 0) { acc }
    else { _number(tl(eps), i + 1, list_append(acc, map_put(hd(eps), 'slot', i))) }
}

fun _done_lines(lines, acc) {
    if (length(lines) == 0) { acc }
    else {
        line = hd(lines)
        row = if (line == "") { nil } else { json_decode(line) }
        next = if (row == nil || typeof(row) != "map" || map_get(row, 'task_id') == nil) { acc }
               else { map_put(acc, episode_key(map_get(row, 'task_id'), map_get(row, 'sample')), 'true') }
        _done_lines(tl(lines), next)
    }
}

fun _or(v, dflt) { if (v == nil) { dflt } else { v } }

fun _bump(m, k) { map_put(m, k, _opt(m, k, 0) + 1) }

fun _clip(s, n) {
    if (s == nil) { nil }
    else { if (string_length(s) > n) { string_sub(s, 0, n) ++ "..." } else { s } }
}

fun _messages(prompt) {
    if (typeof(prompt) == "string") { [%{role: "user", content: prompt}] } else { prompt }
}

fun _left_ms(ctx) {
    d = map_get(ctx, 'deadline')
    if (d == 0) { 0 } else { l = d - timestamp() ; if (l < 1) { 1 } else { l } }
}

# Trajectory recording. Each episode slot has two ETS keys: "<slot>:cur", the
# model / tool calls since the last closed step, and "<slot>:steps", the
# closed steps. ETS (not process state) so the coordinator can read a killed
# episode's partial trajectory.
fun _push(ctx, what, ev) {
    t = map_get(ctx, 'table')
    k = f"{map_get(ctx, 'slot')}:cur"
    cur = _or(ets_get(t, k), %{model: [], tools: []})
    cur = if (what == "model") { map_put(cur, 'model', list_append(map_get(cur, 'model'), ev)) }
          else { map_put(cur, 'tools', list_append(map_get(cur, 'tools'), ev)) }
    ets_put(t, k, cur)
}

fun _close_step(ctx, fields) {
    t = map_get(ctx, 'table')
    slot = map_get(ctx, 'slot')
    cur = _or(ets_get(t, f"{slot}:cur"), %{model: [], tools: []})
    ets_delete(t, f"{slot}:cur")
    steps = _or(ets_get(t, f"{slot}:steps"), [])
    ets_put(t, f"{slot}:steps", list_append(steps, map_merge(cur, fields)))
}

# Drain a slot's recorded steps (an open step, if any, is closed as-is).
fun _take_steps(table, slot) {
    steps = _or(ets_get(table, f"{slot}:steps"), [])
    cur = ets_get(table, f"{slot}:cur")
    ets_delete(table, f"{slot}:steps")
    ets_delete(table, f"{slot}:cur")
    if (cur == nil) { steps } else { list_append(steps, cur) }
}

# Runs in the task_stream worker. Returns {reward, info}, or raises; every
# raise is classified here so the worker returns a cell either way.
fun _episode(env, ep, base) {
    slot = map_get(ep, 'slot')
    started = timestamp()
    tmo = map_get(base, 'episode_timeout_ms')
    ctx = map_merge(base, %{task: map_get(ep, 'task'), task_id: map_get(ep, 'task_id'),
                            sample: map_get(ep, 'sample'), seed: map_get(ep, 'seed'),
                            slot: slot, started_ms: started,
                            deadline: if (tmo > 0) { started + tmo } else { 0 }})
    ets_put(map_get(base, 'table'), f"{slot}:t0", started)
    try {
        r = if (map_get(env, 'run') != nil) { _run_single(env, ctx) } else { _run_steps(env, ctx) }
        {'done', elem(r, 0), elem(r, 1)}
    } catch e {
        case e {
            {'rollout', kind, msg, detail} -> {'failed', kind, msg, detail}
            other -> {'failed', 'env_crash', to_string(other), nil}
        }
    }
}

fun _run_single(env, ctx) {
    f = map_get(env, 'run')
    r = f(map_get(ctx, 'task'), ctx)
    reward = if (typeof(r) == "tuple") { elem(r, 0) } else { r }
    info = if (typeof(r) == "tuple") { elem(r, 1) } else { nil }
    _close_step(ctx, %{t: 0, reward: reward})
    {reward, info}
}

fun _run_steps(env, ctx) {
    reset = map_get(env, 'reset')
    task = map_get(ctx, 'task')
    st = _timed(ctx, "reset", fn() { reset(task, ctx) })
    loop = _step_loop(env, ctx, elem(st, 0), elem(st, 1), 0, 0)
    state = elem(loop, 0)
    score = map_get(env, 'score')
    reward = if (score == nil) { elem(loop, 1) } else { _timed(ctx, "score", fn() { score(state, ctx) }) }
    {reward, %{steps: elem(loop, 2)}}
}

# → {state, summed reward, steps taken}
fun _step_loop(env, ctx, state, obs, t, total) {
    if (t >= map_get(ctx, 'max_steps')) { {state, total, t} }
    else {
        policy = map_get(env, 'policy')
        step = map_get(env, 'step')
        action = policy(obs, ctx)
        r = _timed(ctx, "step", fn() { step(state, action, ctx) })
        reward = elem(r, 2)
        done = elem(r, 3)
        _close_step(ctx, %{t: t, action: action, obs: elem(r, 1), reward: reward, done: done})
        nt = total + (if (reward == nil) { 0 } else { reward })
        if (done == 'true') { {elem(r, 0), nt, t + 1} }
        else { _step_loop(env, ctx, elem(r, 0), elem(r, 1), t + 1, nt) }
    }
}

# Run one env call under step_timeout_ms (0: inline, no limit). The call runs
# in a monitored helper process, so a call that never returns is killed and
# the episode ends as step_timeout instead of eating its whole deadline.
fun _timed(ctx, what, f) {
    ms = map_get(ctx, 'step_timeout_ms')
    if (ms <= 0) { f() }
    else {
        me = self()
        pr = spawn_monitor(fn() {
            r = try { {'ok', f()} } catch e { {'raised', e} }
            send(me, {'rollout_call', self(), r})
        })
        pid = elem(pr, 0)
        ref = elem(pr, 1)
        receive {
            {'rollout_call', p, r} when p == pid ->
                demonitor(ref)
                receive { {'DOWN', rr, _, _, _} when rr == ref -> 'ok' after 0 -> 'ok' }
                case r { {'ok', v} -> v ; {'raised', e} -> error(e) }
            {'DOWN', rr, _, _, why} when rr == ref ->
                fail('env_crash', f"{what}: {to_string(why)}", nil)
            after ms {
                exit_proc(pid, 'kill')
                demonitor(ref)
                fail('step_timeout', f"{what} ran past {ms} ms", nil)
            }
        }
    }
}

# The JSONL row for one finished episode (on the coordinator).
fun _row(table, ep, cell) {
    slot = map_get(ep, 'slot')
    started = ets_get(table, f"{slot}:t0")
    ets_delete(table, f"{slot}:t0")
    steps = _take_steps(table, slot)
    now = timestamp()
    base = %{task_id: map_get(ep, 'task_id'), sample: map_get(ep, 'sample'),
             seed: map_get(ep, 'seed'), started_ms: started,
             duration_ms: if (started == nil) { nil } else { now - started },
             steps: steps}
    fields = case cell {
        {'ok', {'done', reward, info}} ->
            %{status: "ok", kind: "done", reward: reward, info: info, error: nil, error_kind: nil}
        {'ok', {'failed', kind, msg, detail}} ->
            %{status: "error", kind: to_string(kind), reward: nil, info: nil, error: msg,
              error_kind: if (detail == nil) { nil } else { to_string(detail) }}
        {'error', 'timeout'} ->
            %{status: "error", kind: "timeout", reward: nil, info: nil,
              error: "episode ran past episode_timeout_ms", error_kind: nil}
        {'error', reason} ->
            # A panic in env code reaches task_stream as its message (a
            # string); anything else killed the episode process from outside.
            if (typeof(reason) == "string") {
                %{status: "error", kind: "env_crash", reward: nil, info: nil, error: reason, error_kind: nil}
            } else {
                %{status: "error", kind: "infra", reward: nil, info: nil, error: to_string(reason), error_kind: nil}
            }
    }
    map_merge(base, fields)
}
