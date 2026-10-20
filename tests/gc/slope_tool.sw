# slope_tool.sw — memory-slope probe for tool_call (tool registry).
# A long-lived process calls a hot-loaded tool in a tight loop. Each call builds
# per-call garbage inside the tool (a list of strings + a map) and returns only a
# small int. The live set is tiny, so peak RSS must be FLAT in the number of calls.
#
# It also checks that an ETS entry a tool wrote survives the writer's exit. The
# writer's per-call region is adopted by the writer process and freed when it
# exits, so the store must own its values (a raw pointer would read back nil).
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

# A process that writes through a tool, then exits (freeing its region).
fun put_map(tid) { tool_call("stash", tid, "payload") ; send(self(), 'done') }

fun churn_junk(n, acc) {
    if (n <= 0) { acc }
    else { churn_junk(n - 1, acc + length("junk" ++ to_string(n))) }
}

fun ets_outlives_writer() {
    tid = tool_call("mk")
    spawn(put_map(tid))
    receive { 'done' -> { 0 } after 5000 { 0 } }
    sleep(50)
    churn_junk(2000, 0)
    got = tool_call("fetch", tid)
    if (got == %{"tag" => "payload", "body" => "payloadpayload"}) { 0 }
    else { print("FAIL ets entry lost its writer's value: " ++ to_string(got)) ; 1 }
}

fun main() {
    n = job_count(os_args())
    tool_define("churn", "module T\nfun run(i, k) { s = \"0123456789abcdef\" ++ to_string(i); l = [s, s ++ s, s ++ s ++ s]; m = %{\"k\" => s, \"n\" => i}; length(l) + k }")
    tool_define("mk", "module T\nfun run() { ets_new() }")
    tool_define("stash", "module T\nfun run(t, tag) { ets_put(t, 7, %{\"tag\" => tag, \"body\" => tag ++ tag}); 'ok' }")
    tool_define("fetch", "module T\nfun run(t) { ets_get(t, 7) }")
    print("slope_tool calls=" ++ to_string(n))
    total = loop(n, 0)
    print("slope_tool DONE total=" ++ to_string(total))
    ets_fails = ets_outlives_writer()
    # Each call returns length([s, s++s, s++s++s]) + 3 == 6: results must survive the call.
    if (total != 6 * n) { print("FAIL slope_tool total=" ++ to_string(total)); 1 }
    else if (ets_fails > 0) { print("FAIL slope_tool ets"); sys_exit(1) }
    else { print("PROBE_OK"); 0 }
}
