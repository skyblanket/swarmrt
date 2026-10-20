# slope_tool.sw — memory-slope probe for tool_call (tool registry).
# A long-lived process calls a hot-loaded tool in a tight loop. Each call builds
# per-call garbage inside the tool (a list of strings + a map) and returns only a
# small int. The live set is tiny, so peak RSS must be FLAT in the number of calls.
#
# Usage: slope_tool <calls>   (default 4000). Driven by scripts/gc_slope.sh.
module Main

fun job_count(a) { if (length(a) >= 2) { to_int(hd(tl(a))) } else { 4000 } }

fun loop(calls, acc) {
    if (calls <= 0) { acc }
    else {
        r = tool_call("churn", calls, 3)
        loop(calls - 1, acc + r)
    }
}

fun main() {
    n = job_count(os_args())
    tool_define("churn", "module T\nfun run(i, k) { s = \"0123456789abcdef\" ++ to_string(i); l = [s, s ++ s, s ++ s ++ s]; m = %{\"k\" => s, \"n\" => i}; length(l) + k }")
    print("slope_tool calls=" ++ to_string(n))
    total = loop(n, 0)
    print("slope_tool DONE total=" ++ to_string(total))
    # Each call returns length([s, s++s, s++s++s]) + 3 == 6: results must survive the call.
    if (total != 6 * n) { print("FAIL slope_tool total=" ++ to_string(total)); 1 }
    else { print("PROBE_OK"); 0 }
}
