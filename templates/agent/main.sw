module Main

# Runs every task in tasks.txt as its own agent process: at most 8 at a
# time, each with a 30 s deadline. A task whose agent crashes or hangs ends
# as one error line; the others finish.
#
#   LLM_PROVIDER=ollama LLM_MODEL=qwen2.5 ./bin/{{name}}
#   LLM_URL=https://.../v1/chat/completions LLM_API_KEY=... ./bin/{{name}}

import Std
import Agent

fun main() {
    tasks = filter(string_split(file_read("tasks.txt"), "\n"), fn(t) { string_trim(t) != "" })
    results = Std.task_stream(tasks, fn(t) { Agent.answer(t, %{}) },
                              %{max_concurrency: 8, timeout_ms: 30000})
    report(tasks, results, 0, 0)
}

fun report(tasks, results, i, failed) {
    if (length(tasks) == 0) {
        print(f"{i - failed}/{i} tasks answered")
        if (failed > 0) { sys_exit(1) }
    } else {
        r = hd(results)
        line = case r {
            {'ok', answer} -> "ok     " ++ hd(tasks) ++ " -> " ++ answer
            {'error', why} -> "error  " ++ hd(tasks) ++ " -> " ++ to_string(why)
            _ -> "error  " ++ hd(tasks) ++ " -> timed out"
        }
        print(line)
        more = if (elem(r, 0) == 'ok') { 0 } else { 1 }
        report(tl(tasks), tl(results), i + 1, failed + more)
    }
}
