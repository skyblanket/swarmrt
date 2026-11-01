module Test_rollout

# lib/Rollout.sw: every episode outcome is typed and kept apart from a reward
# of 0, one JSONL row per episode that round-trips through json_decode,
# samples_per_task, a policy-driven multi-turn env, and resume skipping rows
# already written. (Resume after SIGKILL: tests/sw/run_rollout_resume.sh.)
# A local server (below) plays the model; Rollout points at it via opts.llm.

import Std
import Rollout

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun listen_and_serve(port) {
    http_listen(port)
    serve()
}

# "/ok" echoes the last message's content upper-cased; "/bad" is a 400.
fun serve() {
    receive {
        {'http_request', conn, _m, path, _h, body} ->
            if (path == "/bad") {
                http_respond(conn, 400, "Content-Type: application/json\r\n",
                             "{\"error\":{\"message\":\"no such model\"}}")
            } else {
                msgs = map_get(json_decode(body), 'messages')
                said = map_get(Std.last(msgs), 'content')
                http_respond(conn, 200, "Content-Type: application/json\r\n",
                    json_encode(%{choices: [%{finish_reason: "stop",
                        message: %{role: "assistant", content: string_upper(said)}}],
                        usage: %{prompt_tokens: 3, completion_tokens: 1}}))
            }
            serve()
        _ -> serve()
    }
}

fun spin(i) { spin(i + 1) }

# One single-shot env; the task's "mode" picks the outcome.
fun run_task(task, ctx) {
    mode = map_get(task, 'mode')
    cond {
        mode == "ok" -> {
            out = Rollout.llm(ctx, f"answer {map_get(task, 'id')}")
            {if (out == string_upper(f"answer {map_get(task, 'id')}")) { 1 } else { 0 }, %{out: out}}
        }
        mode == "zero" -> { {0, nil} }
        mode == "hang" -> { Rollout.llm(ctx, "before the hang") ; sleep(60000) ; {1, nil} }
        mode == "spin" -> { spin(0) }
        mode == "tool" -> {
            Rollout.tool(ctx, "sh", ["-c", "sleep 30 & wait"], %{timeout_ms: 300})
            {1, nil}
        }
        mode == "tool_ok" -> {
            t = Rollout.tool(ctx, "sh", ["-c", "echo hi"], %{})
            {if (string_trim(elem(t, 1)) == "hi") { 1 } else { 0 }, nil}
        }
        mode == "panic" -> { panic("env blew up") }
        mode == "raise" -> { error("env raised") }
        mode == "model" -> {
            Rollout.llm_with(ctx, "x", %{url: "http://127.0.0.1:9431/bad"})
            {1, nil}
        }
        mode == "kill" -> { exit_proc(self(), 'kill') ; sleep(1000) ; {1, nil} }
        _ -> { {0, nil} }
    }
}

# A multi-turn env: count to the task's target; the policy asks the model to
# echo the next number. "slow" makes step 1 hang past step_timeout_ms.
fun counter_env() {
    %{reset: fn(task, ctx) { {%{n: 0, target: map_get(task, 'target'), slow: map_get(task, 'slow')}, "0"} },
      policy: fn(obs, ctx) { Rollout.llm(ctx, f"{to_int(obs) + 1}") },
      step: fn(state, action, ctx) {
          if (map_get(state, 'slow') == 'true' && map_get(state, 'n') == 1) { sleep(60000) }
          n = to_int(action)
          st = map_put(state, 'n', n)
          {st, to_string(n), 1, if (n >= map_get(state, 'target')) { 'true' } else { 'false' }}
      },
      score: fn(state, ctx) { if (map_get(state, 'n') == map_get(state, 'target')) { 10 } else { 0 } }}
}

fun tmp_dir() {
    t = getenv("TMPDIR")
    if (t == nil || t == "") { "/tmp" } else { t }
}

fun read_rows(path) {
    lines = filter(string_split(file_read(path), "\n"), fn(l) { l != "" })
    Std.map(lines, fn(l) { json_decode(l) })
}

fun row_for(rows, id) { Std.find(rows, fn(r) { map_get(r, 'task_id') == id }) }

fun kind_of(rows, id) { map_get(row_for(rows, id), 'kind') }

fun main() {
    spawn(listen_and_serve(9431))
    sleep(300)
    llm = %{url: "http://127.0.0.1:9431/ok", api_key: "k", retries: 0}
    dir = tmp_dir() ++ "/sw_rollout_test_" ++ uuid()
    shell("mkdir -p " ++ dir)
    out = dir ++ "/single.jsonl"
    f = 0

    # --- 1. Every outcome kind, single-shot.
    modes = ["ok", "zero", "hang", "spin", "tool", "tool_ok", "panic", "raise", "model", "kill"]
    tasks = Std.map(modes, fn(m) { %{id: m, mode: m} })
    t0 = timestamp()
    s = Rollout.run(tasks, %{run: fn(task, ctx) { run_task(task, ctx) }},
                    %{max_concurrency: 10, episode_timeout_ms: 1500, out: out, llm: llm})
    el = timestamp() - t0
    rows = read_rows(out)
    f = f + assert_eq("rows_one_per_episode", length(rows), 10)
    f = f + assert_eq("summary_ran", map_get(s, 'ran'), 10)
    f = f + assert_eq("summary_ok", map_get(s, 'ok'), 3)
    f = f + assert_eq("kind_ok", kind_of(rows, "ok"), "done")
    f = f + assert_eq("reward_ok", map_get(row_for(rows, "ok"), 'reward'), 1)
    f = f + assert_eq("kind_zero", kind_of(rows, "zero"), "done")
    f = f + assert_eq("reward_zero_is_zero", map_get(row_for(rows, "zero"), 'reward'), 0)
    f = f + assert_eq("kind_hang", kind_of(rows, "hang"), "timeout")
    f = f + assert_eq("kind_spin", kind_of(rows, "spin"), "timeout")
    f = f + assert_eq("kind_tool", kind_of(rows, "tool"), "tool_timeout")
    f = f + assert_eq("kind_tool_ok", kind_of(rows, "tool_ok"), "done")
    f = f + assert_eq("kind_panic", kind_of(rows, "panic"), "env_crash")
    f = f + assert_eq("kind_raise", kind_of(rows, "raise"), "env_crash")
    f = f + assert_eq("kind_model", kind_of(rows, "model"), "model_error")
    f = f + assert_eq("model_error_kind", map_get(row_for(rows, "model"), 'error_kind'), "bad_request")
    f = f + assert_eq("kind_kill", kind_of(rows, "kill"), "infra")
    # A failure's reward is null, never 0.
    f = f + assert_eq("failed_reward_null", map_get(row_for(rows, "hang"), 'reward'), nil)
    f = f + assert_eq("failed_status", map_get(row_for(rows, "panic"), 'status'), "error")
    # The deadlines cut the hung episodes: the run is ~1.5 s, not 60.
    f = f + assert_eq("run_bounded", el < 6000, 'true')

    # The trajectory: the model call and the tool call are in the steps.
    okrow = row_for(rows, "ok")
    step0 = hd(map_get(okrow, 'steps'))
    call0 = hd(map_get(step0, 'model'))
    f = f + assert_eq("traj_model_output", map_get(call0, 'output'), "ANSWER OK")
    f = f + assert_eq("traj_model_messages", map_get(hd(map_get(call0, 'messages')), 'content'), "answer ok")
    f = f + assert_eq("traj_model_usage", map_get(map_get(call0, 'usage'), 'prompt_tokens'), 3)
    f = f + assert_eq("traj_step_reward", map_get(step0, 'reward'), 1)
    tool0 = hd(map_get(hd(map_get(row_for(rows, "tool"), 'steps')), 'tools'))
    f = f + assert_eq("traj_tool_timed_out", map_get(tool0, 'timed_out'), 'true')
    f = f + assert_eq("traj_tool_exit", map_get(tool0, 'exit_code'), 124)
    f = f + assert_eq("traj_info", map_get(map_get(okrow, 'info'), 'out'), "ANSWER OK")
    # A timed-out (killed) episode keeps the partial trajectory it had recorded.
    hang_call = hd(map_get(hd(map_get(row_for(rows, "hang"), 'steps')), 'model'))
    f = f + assert_eq("traj_partial_on_timeout", map_get(hang_call, 'output'), "BEFORE THE HANG")
    # A row round-trips: decode(encode(decode(line))) == decode(line).
    f = f + assert_eq("jsonl_roundtrip", json_decode(json_encode(okrow)), okrow)

    # --- 2. Resume skips episodes already in `out` (any outcome).
    s2 = Rollout.run(tasks, %{run: fn(task, ctx) { run_task(task, ctx) }},
                     %{max_concurrency: 10, episode_timeout_ms: 1500, out: out, llm: llm, resume: 'true'})
    f = f + assert_eq("resume_skipped_all", map_get(s2, 'skipped'), 10)
    f = f + assert_eq("resume_ran_none", map_get(s2, 'ran'), 0)
    f = f + assert_eq("resume_no_dup_rows", length(read_rows(out)), 10)

    # --- 3. samples_per_task: 2 tasks x 3 samples, distinct seeds per sample.
    out3 = dir ++ "/samples.jsonl"
    s3 = Rollout.run([%{id: "a", mode: "zero"}, %{id: "b", mode: "zero"}],
                     %{run: fn(task, ctx) { {map_get(ctx, 'seed'), nil} }},
                     %{samples_per_task: 3, seed: 100, out: out3})
    rows3 = read_rows(out3)
    f = f + assert_eq("samples_rows", length(rows3), 6)
    f = f + assert_eq("samples_summary", map_get(s3, 'episodes'), 6)
    a_seeds = Std.map(filter(rows3, fn(r) { map_get(r, 'task_id') == "a" }), fn(r) { map_get(r, 'seed') })
    f = f + assert_eq("samples_seeds", Std.sort(a_seeds), [100, 101, 102])
    f = f + assert_eq("samples_reward_is_seed", Std.all(rows3, fn(r) {
        if (map_get(r, 'reward') == map_get(r, 'seed')) { 'true' } else { 'false' } }), 'true')

    # --- 4. Multi-turn: reset / policy / step / score, and step_timeout.
    out4 = dir ++ "/multi.jsonl"
    s4 = Rollout.run([%{id: "count3", target: 3}, %{id: "slow", target: 3, slow: 'true'}],
                     counter_env(),
                     %{out: out4, llm: llm, step_timeout_ms: 400, episode_timeout_ms: 5000})
    rows4 = read_rows(out4)
    c3 = row_for(rows4, "count3")
    f = f + assert_eq("multi_kind", map_get(c3, 'kind'), "done")
    f = f + assert_eq("multi_score", map_get(c3, 'reward'), 10)
    f = f + assert_eq("multi_steps", length(map_get(c3, 'steps')), 3)
    f = f + assert_eq("multi_step_action", map_get(Std.nth(map_get(c3, 'steps'), 2), 'action'), "3")
    f = f + assert_eq("multi_step_done", map_get(Std.nth(map_get(c3, 'steps'), 2), 'done'), 'true')
    f = f + assert_eq("multi_step_timeout", kind_of(rows4, "slow"), "step_timeout")

    # --- 5. Without resume, an existing `out` is refused rather than appended to
    # (or clobbered): see Rollout.run. Not exercised here because it panics.

    shell("rm -rf " ++ dir)
    if (f == 0) { print("OK test_rollout") ; sys_exit(0) }
    else { print(f"FAIL test_rollout {f} failed") ; sys_exit(1) }
}
