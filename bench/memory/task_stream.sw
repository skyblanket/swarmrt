module Main
import Std

# N task_stream workers that all wait: "llm" on a model call that never
# answers (the mock server hangs EP ids % 100 == 7 on turn 0), "idle" in a
# receive. Sampled from outside while they wait.
fun ids(i, acc) { if (i < 0) { acc } else { ids(i - 1, [i | acc]) } }

fun run(n, mode, url) {
    llm = %{url: url, max_tokens: 16, min_chars: 1}
    Std.task_stream(ids(n - 1, []), fn(i) {
        if (mode == "llm") { llm_complete(f"EP {7 + 100 * i} TURN 0: wait", llm) }
        else { receive { 'never' -> 0 } }
    }, %{max_concurrency: n})
}

fun main() {
    n = to_int(getenv("N"))
    spawn(run(n, getenv("MODE"), getenv("URL")))
    print("READY")
    receive { 'never' -> 0 }
}
