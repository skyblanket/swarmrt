module Rollout_runner

# Eval-style rollout benchmark: N episodes, each two model turns and one tool
# call, then a grader. Faults are injected by the mock server and by episode id
# (see README.md). The runner is Rollout.run (lib/Rollout.sw): the deadlines,
# typed failures, trajectory rows and resume are the library's, not this file's.
#
# Env: BENCH_N, BENCH_CONC, BENCH_URL, BENCH_OUT (JSONL), BENCH_SCENARIO
#      ("api", "runaway" or "clean"). Episodes already in BENCH_OUT are skipped (resume).

import Std
import Rollout

fun env_int(name, dflt) {
    v = getenv(name)
    if (v == nil || v == "") { dflt } else { to_int(v) }
}

fun env_str(name, dflt) {
    v = getenv(name)
    if (v == nil || v == "") { dflt } else { v }
}

# The tool: prints id * 2. Episodes with id % 100 == 13 run a tool that hangs
# and leaves a grandchild behind (`sleep 300` under `sh`).
fun tool_cmd(id, scenario) {
    if (scenario != "clean" && id % 100 == 13) { "sleep 300 & wait" } else { "echo $((" ++ to_string(id) ++ " * 2))" }
}

fun spin(i) { spin(i + 1) }

# The grader. id % 100 == 29 crashes; in the "runaway" scenario
# id % 100 == 53 never returns (a CPU loop).
fun grade(id, answer, scenario) {
    if (scenario != "clean" && id % 100 == 29) { panic("grader crashed on episode " ++ to_string(id)) }
    if (scenario == "runaway" && id % 100 == 53) { spin(0) }
    answer == "ANSWER " ++ to_string(id * 2)
}

fun episode(task, ctx, scenario) {
    id = map_get(task, 'id')
    r0 = string_trim(Rollout.llm(ctx, f"EP {id} TURN 0: pick a tool"))
    if (string_starts_with(r0, "TOOL") == 'false') { panic("bad turn 0 reply: " ++ r0) }
    t = Rollout.tool(ctx, "sh", ["-c", tool_cmd(id, scenario)], %{timeout_ms: 5000})
    r1 = string_trim(Rollout.llm(ctx, f"EP {id} TURN 1: RESULT {string_trim(elem(t, 1))}"))
    {if (grade(id, r1, scenario)) { 1 } else { 0 }, nil}
}

fun main() {
    n = env_int("BENCH_N", 1000)
    scenario = env_str("BENCH_SCENARIO", "api")
    tasks = Std.map(Std.range(0, n), fn(i) { %{id: i} })
    t0 = timestamp()
    s = Rollout.run(tasks, %{run: fn(task, ctx) { episode(task, ctx, scenario) }}, %{
        max_concurrency: env_int("BENCH_CONC", 128), episode_timeout_ms: 15000,
        out: env_str("BENCH_OUT", "swarmrt_results.jsonl"), resume: 'true',
        llm: %{url: env_str("BENCH_URL", "http://127.0.0.1:18500/v1/chat/completions"),
               max_tokens: 16}})
    eprint(f"swarmrt: ran {map_get(s, 'ran')} of {n} episodes ({map_get(s, 'skipped')} already done) in {timestamp() - t0} ms")
}
