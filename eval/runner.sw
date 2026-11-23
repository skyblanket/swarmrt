module Eval_runner

# eval/runner.sw — the sw code-generation eval (same prompts, models and check
# as runner.sh) on Rollout.run: every (prompt x model) cell is a task, sampled
# EVAL_K times; each episode asks the model for a program, builds it with swc,
# runs it, and scores 1 when its output matches the prompt's expected output.
#
#   cd eval && ../bin/swc build runner.sw -o .runner && ./.runner
#
# Env: EVAL_K (samples per cell, default 5), EVAL_ONLY (run only prompts whose
# file name contains it, e.g. "03_"), SWC (default ../bin/swc), EVAL_CONC (episodes in flight,
# default 8), EVAL_OUT (trajectory JSONL, default results/rollout_<ts>.jsonl;
# an existing file is resumed). Models whose key env var is unset are skipped.
# Unlike runner.sh, a model or infra failure is reported as its own kind, not
# as a failed sample, and a killed run resumes where it stopped.

import Std
import Rollout

fun env_int(name, dflt) {
    v = getenv(name)
    if (v == nil || v == "") { dflt } else { to_int(v) }
}

fun tmp_dir() {
    t = getenv("TMPDIR")
    if (t == nil || t == "") { "/tmp" } else { t }
}

# Drop lines up to and including the first one that starts with `prefix`.
fun after_line(lines, prefix) {
    if (length(lines) == 0) { [] }
    else { if (string_starts_with(hd(lines), prefix) == 'true') { tl(lines) } else { after_line(tl(lines), prefix) } }
}

# Lines up to (not including) the next ``` fence.
fun until_fence(lines, acc) {
    if (length(lines) == 0) { acc }
    else { if (string_starts_with(hd(lines), "```") == 'true') { acc } else { until_fence(tl(lines), list_append(acc, hd(lines))) } }
}

# The body of the first fenced block whose opening line starts with `open`.
fun block(lines, open) { until_fence(after_line(lines, open), []) }

fun normalize(text) {
    lines = filter(string_split(text, "\n"), fn(l) { string_starts_with(l, "[SwarmRT]") == 'false' })
    string_trim(Std.join(Std.map(lines, fn(l) { string_trim(l) }), "\n"))
}

# The program in a model reply: its first ```sw block, else its first fenced
# block, else the whole reply.
fun extract_program(reply) {
    lines = string_split(reply, "\n")
    sw = block(lines, "```sw")
    any = block(lines, "```")
    cond {
        length(sw) > 0 -> Std.join(sw, "\n")
        length(any) > 0 -> Std.join(any, "\n")
        _ -> reply
    }
}

fun episode(task, ctx, swc, system) {
    reply = Rollout.llm_with(ctx, [%{role: "system", content: system},
                                   %{role: "user", content: map_get(task, 'prompt')}],
                             %{url: map_get(task, 'endpoint'), model: map_get(task, 'model'),
                               api_key: map_get(task, 'key'), max_tokens: 16384,
                               temperature: 1.0, timeout_ms: 120000})
    dir = f"{tmp_dir()}/sw_eval_{uuid()}"
    shell("mkdir -p " ++ dir)
    file_write(dir ++ "/program.sw", extract_program(reply))
    # Through sh only for 2>&1, so compile errors land in the trajectory; the
    # paths are positional args, never pasted into the script.
    b = Rollout.tool(ctx, "sh", ["-c", "exec \"$0\" build \"$1\" -o \"$2\" 2>&1", swc,
                                 dir ++ "/program.sw", dir ++ "/program"], %{timeout_ms: 120000})
    reward = if (elem(b, 0) != 0) { 0 }
             else {
                 r = Rollout.tool(ctx, dir ++ "/program", [], %{timeout_ms: 10000})
                 if (normalize(elem(r, 1)) == normalize(map_get(task, 'expected'))) { 1 } else { 0 }
             }
    shell("rm -rf " ++ dir)
    {reward, %{model_id: map_get(task, 'model_id'), prompt_id: map_get(task, 'prompt_id'),
               compiled: elem(b, 0) == 0}}
}

fun prompt_files(args_dir) {
    names = Std.sort(filter(file_list(args_dir), fn(n) { string_ends_with(n, ".md") }))
    Std.map(names, fn(n) { args_dir ++ "/" ++ n })
}

fun base_name(path) { string_replace(Std.last(string_split(path, "/")), ".md", "") }

# One task per (prompt, model) whose key is set.
fun cells(prompts, models) {
    Std.reduce(prompts, [], fn(acc, p) {
        text = file_read(p)
        expected = Std.join(block(after_line(string_split(text, "\n"), "## Expected output"), "```"), "\n")
        acc ++ Std.map(models, fn(m) {
            %{id: f"{base_name(p)}/{map_get(m, 'id')}", prompt_id: base_name(p),
              model_id: map_get(m, 'id'), prompt: text, expected: expected,
              endpoint: map_get(m, 'endpoint'), model: map_get(m, 'model_field'),
              key: getenv(map_get(m, 'key_env'))}
        })
    })
}

fun report(out, models) {
    rows = Std.map(filter(string_split(file_read(out), "\n"), fn(l) { l != "" }), fn(l) { json_decode(l) })
    Std.each(models, fn(m) {
        mine = filter(rows, fn(r) { string_ends_with(map_get(r, 'task_id'), "/" ++ map_get(m, 'id')) })
        ok = filter(mine, fn(r) { map_get(r, 'kind') == "done" })
        passed = filter(ok, fn(r) { map_get(r, 'reward') == 1 })
        failed = length(mine) - length(ok)
        print(f"  {map_get(m, 'label')}: {length(passed)}/{length(ok)} passed" ++
              (if (failed > 0) { f", {failed} not scored (model or infra failures)" } else { "" }))
    })
}

fun main() {
    k = env_int("EVAL_K", 5)
    root = "."
    swc = case getenv("SWC") { nil -> "../bin/swc" ; "" -> "../bin/swc" ; v -> v }
    system = file_read("system_prompt.md")
    all_models = map_get(json_decode(file_read("models.json")), 'models')
    models = filter(all_models, fn(m) {
        key = getenv(map_get(m, 'key_env'))
        if (key == nil || key == "") { print(f"  SKIP {map_get(m, 'id')} (${map_get(m, 'key_env')} unset)") ; 'false' }
        else { 'true' }
    })
    if (length(models) == 0) { print("no model has its key set; nothing to run") ; sys_exit(2) }
    only = getenv("EVAL_ONLY")
    prompts = filter(prompt_files(root ++ "/prompts"), fn(p) {
        only == nil || only == "" || string_contains(p, only) == 'true'
    })
    out = case getenv("EVAL_OUT") { nil -> f"results/rollout_{timestamp()}.jsonl" ; "" -> f"results/rollout_{timestamp()}.jsonl" ; o -> o }
    shell("mkdir -p results")
    s = Rollout.run(cells(prompts, models), %{run: fn(task, ctx) { episode(task, ctx, swc, system) }},
                    %{max_concurrency: env_int("EVAL_CONC", 8), samples_per_task: k,
                      episode_timeout_ms: 300000, out: out, resume: 'true',
                      llm: %{retries: 4}})
    print(f"ran {map_get(s, 'ran')} episodes ({map_get(s, 'skipped')} resumed) in {map_get(s, 'wall_ms')} ms: {to_string(map_get(s, 'kinds'))}")
    report(out, models)
    print(f"trajectories: {out}")
}
