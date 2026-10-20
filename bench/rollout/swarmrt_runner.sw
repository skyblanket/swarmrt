module Rollout_runner

# Eval-style rollout benchmark: N episodes, each two model turns and one tool
# call, then a grader. Faults are injected by the mock server and by episode id
# (see README.md); this runner only uses what SwarmRT ships.
#
# Env: BENCH_N, BENCH_CONC, BENCH_URL, BENCH_OUT (JSONL), BENCH_SCENARIO
#      ("api", "runaway" or "clean"). Episodes already in BENCH_OUT are skipped (resume).

import Std

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

fun record(out, id, status, kind, correct) {
    file_append(out, json_encode(%{id: id, status: status, kind: kind, correct: correct}) ++ "\n")
}

fun episode(id, llm, out, scenario) {
    r0 = string_trim(llm_complete(f"EP {id} TURN 0: pick a tool", llm))
    if (string_starts_with(r0, "TOOL") == 'false') { panic("bad turn 0 reply: " ++ r0) }
    t = exec_managed("sh", ["-c", tool_cmd(id, scenario)], %{timeout_ms: 5000})
    if (elem(t, 0) == 124) {
        record(out, id, "error", "tool_timeout", 'false')
        'tool_timeout'
    } else {
        result = string_trim(elem(t, 1))
        r1 = string_trim(llm_complete(f"EP {id} TURN 1: RESULT {result}", llm))
        ok = grade(id, r1, scenario)
        record(out, id, "ok", "done", ok)
        ok
    }
}

fun collect_ids(lines, acc) {
    if (length(lines) == 0) { acc }
    else {
        line = hd(lines)
        row = if (line == "") { nil } else { json_decode(line) }
        next = if (row == nil) { acc } else { map_put(acc, map_get(row, 'id'), 'true') }
        collect_ids(tl(lines), next)
    }
}

fun done_ids(out) {
    if (file_exists(out) == 'false') { %{} }
    else { collect_ids(string_split(file_read(out), "\n"), %{}) }
}

# task_stream results come back in input order; record the failures it caught
# (grader crashes, deadline timeouts) that the episode itself could not write.
fun record_failures(out, ids, results) {
    if (length(ids) == 0) { 'ok' }
    else {
        id = hd(ids)
        case hd(results) {
            {'error', 'timeout'} -> record(out, id, "error", "timeout", 'false')
            {'error', reason}    -> record(out, id, "error", "crash", 'false')
            _                    -> 'ok'
        }
        record_failures(out, tl(ids), tl(results))
    }
}

fun main() {
    n = env_int("BENCH_N", 1000)
    conc = env_int("BENCH_CONC", 128)
    url = env_str("BENCH_URL", "http://127.0.0.1:18500/v1/chat/completions")
    out = env_str("BENCH_OUT", "swarmrt_results.jsonl")
    scenario = env_str("BENCH_SCENARIO", "api")
    llm = %{url: url, max_tokens: 16, min_chars: 1}

    seen = done_ids(out)
    todo = filter(Std.range(0, n), fn(i) { map_get(seen, i) == nil })
    eprint(f"swarmrt: {length(todo)} of {n} episodes to run, concurrency {conc}")
    t0 = timestamp()
    rs = Std.task_stream(todo, fn(id) { episode(id, llm, out, scenario) },
                         %{max_concurrency: conc, timeout_ms: 15000})
    record_failures(out, todo, rs)
    eprint(f"swarmrt: finished in {timestamp() - t0} ms")
}
