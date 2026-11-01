# Rollout.sw: eval and RL rollout runs

`lib/Rollout.sw` runs episodes of an environment against a model API: one
isolated process per episode, a deadline on each, typed failures that never
fail the run, one trajectory row per episode in a JSONL file, and resumable
runs. It is built on `Std.task_stream_each` (with `on_result`), `llm_request`
and `exec_managed`.

```sw
import Rollout

fun main() {
    tasks = [%{id: "add-1", q: "2+2?", a: "4"}, %{id: "add-2", q: "3+5?", a: "8"}]
    env = %{run: fn(task, ctx) {
        out = string_trim(Rollout.llm(ctx, map_get(task, 'q') ++ " Answer with a number."))
        {if (out == map_get(task, 'a')) { 1 } else { 0 }, %{answer: out}}
    }}
    s = Rollout.run(tasks, env, %{
        max_concurrency: 64, episode_timeout_ms: 60000, samples_per_task: 4, seed: 7,
        out: "rollouts.jsonl", resume: 'true',
        llm: %{provider: "openai", model: "gpt-4o-mini", retries: 4},
        limit: %{rpm: 500, max_concurrency: 32}})
    print(f"{map_get(s, 'ok')} ok, {map_get(s, 'failed')} failed: {to_string(map_get(s, 'kinds'))}")
}
```

## The env contract

An env is a map of functions. Either single-shot:

| Key | Signature | |
|---|---|---|
| `run` | `fn(task, ctx) -> {reward, info}` | one episode; a bare `reward` (no tuple) is accepted too |

or multi-turn, where a policy (usually the model) picks each action:

| Key | Signature | |
|---|---|---|
| `reset` | `fn(task, ctx) -> {state, obs}` | start an episode |
| `policy` | `fn(obs, ctx) -> action` | choose the next action; call the model here |
| `step` | `fn(state, action, ctx) -> {state, obs, reward, done}` | `done` is `'true'` to end the episode |
| `score` | `fn(state, ctx) -> reward` | optional: the episode's reward; else the sum of the step rewards |

The loop stops at `done` or after `max_steps` steps.

`ctx` is plain data: `task`, `task_id`, `sample`, `seed`, `deadline` (an
absolute `timestamp()` in ms, 0 for none) and the run's settings. Pass it to
the helpers below so their calls land in the episode's trajectory:

| Helper | |
|---|---|
| `Rollout.llm(ctx, prompt_or_messages)` → string | `llm_request` with the run's `llm` opts, bounded by the episode deadline. A failure after its retries ends the episode as `model_error`. |
| `Rollout.llm_with(ctx, prompt, extra_opts)` → string | the same, with per-call opts merged over the run's |
| `Rollout.request(ctx, prompt, extra_opts)` → `{'ok', m}` \| `{'error', m}` | `llm_request`'s tagged result, never raising, for envs that handle model errors themselves |
| `Rollout.tool(ctx, cmd, args, opts)` → `{exit_code, stdout}` | `exec_managed`; `timeout_ms` defaults to the run's `tool_timeout_ms` and is capped by the deadline. A timeout kills the tool's process group and ends the episode as `tool_timeout`. |
| `Rollout.fail(kind, message, detail)` | end the episode with a typed failure of your own |

## `Rollout.run(tasks, env, opts)`

`tasks` is a list of maps. A task's `id` names it in the output (its index in
the list when absent); ids must be unique and stable across a resume.

| Opt | Default | |
|---|---|---|
| `max_concurrency` | 16 | episodes in flight at once |
| `episode_timeout_ms` | 0 (none) | per-episode deadline; the episode process is killed at it |
| `step_timeout_ms` | 0 (none) | per env call (`reset`, `step`, `score`); each runs in a helper process that is killed at it |
| `tool_timeout_ms` | 30000 | default `timeout_ms` for `Rollout.tool` |
| `max_steps` | 100 | multi-turn step cap |
| `samples_per_task` | 1 | episodes per task; sample `s` gets seed `seed + s` |
| `seed` | 0 | base seed |
| `llm` | `%{}` | `llm_request` opts for `Rollout.llm` (`url` or `provider`, `model`, `api_key`, `retries`, ...) |
| `limit` | none | `llm_limit` opts (`rpm`, `tpm`, `max_concurrency`) applied to `llm`'s endpoint for the run |
| `out` | none | the trajectory JSONL path |
| `resume` | `'false'` | `'true'`: skip episodes already in `out`. Without it an existing `out` is an error, never silently appended to or overwritten. |
| `on_episode` | none | `fn(row)`, called on the coordinator after each row is written |

It returns `%{episodes, skipped, ran, ok, failed, kinds, wall_ms}`, where
`kinds` counts this call's episodes by outcome.

## Outcomes

Every episode ends with exactly one `kind`. Only `done` has `status: "ok"`.
A failed episode's `reward` is `null`, never 0, so an infra failure is not
scored as a zero-reward sample.

| `kind` | Cause |
|---|---|
| `done` | the env finished; `reward` holds its reward (which may be 0) |
| `timeout` | the episode ran past `episode_timeout_ms` and was killed |
| `step_timeout` | an env call ran past `step_timeout_ms` |
| `tool_timeout` | a `Rollout.tool` call ran past its timeout |
| `model_error` | `Rollout.llm` failed after its retries; `error_kind` is `llm_request`'s kind (`rate_limited`, `server`, `timeout`, `transport`, `bad_request`, `auth`, `bad_response`, `config`) |
| `env_crash` | env code raised (`error(...)`) or panicked |
| `infra` | the episode process died for any other reason (killed from outside, a memory cap) |

## The trajectory JSONL

One line per episode, written by the coordinator as the episode ends:

```json
{"task_id": "add-1", "sample": 0, "seed": 7, "status": "ok", "kind": "done",
 "reward": 1, "info": {"answer": "4"}, "error": null, "error_kind": null,
 "started_ms": 1793500000000, "duration_ms": 812,
 "steps": [
   {"t": 0, "reward": 1,
    "model": [{"messages": [{"role": "user", "content": "2+2? Answer with a number."}],
               "output": "4", "finish_reason": "stop",
               "usage": {"prompt_tokens": 14, "completion_tokens": 1},
               "latency_ms": 640, "attempts": 1, "error": null}],
    "tools": [{"cmd": "sh", "args": ["-c", "..."], "exit_code": 0, "ms": 12,
               "timed_out": false, "output": "..."}]}]}
```

- **Steps.** A multi-turn episode has one step per `env.step` call, with
  `t`, `action`, `obs`, `reward` and `done`, plus the `model` and `tools`
  calls made since the previous step (the policy's model calls land in the
  step they chose the action for). A single-shot episode has one step with
  every call it made.
- **Partial trajectories.** Calls are recorded as they happen, so an episode
  that times out or crashes keeps the calls it made before it ended.
- **Model failures** appear as a `model` entry with `output: null` and
  `error: {kind, status, retry_after_ms}`.
- Tool output is clipped to 4,000 characters, and an error body to 300.

Loading it:

```python
import pandas as pd
df = pd.read_json("rollouts.jsonl", lines=True)
df.groupby("kind").size()
df[df.status == "ok"].groupby("task_id").reward.mean()      # pass@1 over samples
steps = df.explode("steps").dropna(subset=["steps"])         # one row per step
```

For trainers that take conversations (TRL, ART, OpenRLHF), each model call's
`messages` plus its `output` as an assistant turn is the conversation, and the
episode or step `reward` is its reward.

## Resume

Rows are written through `task_stream`'s `on_result` as soon as each episode
ends, failures included. A run killed with SIGKILL therefore re-runs, and
re-bills, at most the episodes that were in flight when it died. Restart it
with `resume: 'true'` on the same `out`: every episode that has a row, with
any outcome, is skipped. A line cut short by the kill is ignored. To re-run
failures, filter them out of the file first.

`tests/sw/run_rollout_resume.sh` checks this: it kills a run with SIGKILL,
restarts it, and verifies one row per episode and that no failure recorded
before the kill ran again.

## Not covered

- Rows are not fsynced: SIGKILL of the runner is safe (the kernel keeps the
  written lines), a power cut is not.
- A native crash of the runner process itself (a segfault, the OS OOM killer)
  ends the run; resume picks it up.
- One `out` file belongs to one run at a time.
