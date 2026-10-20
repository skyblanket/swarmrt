module Test_tool_ets_store

# A tool's ETS write must outlive the tool's region. Each tool_call builds its
# values in a per-call region that the calling process adopts, so when a process
# that wrote through a tool exits, its region is freed. The interpreter's ETS used
# to keep raw pointers into that region, so a later tool reading the entry saw
# freed memory (valgrind: "Invalid read"; here: a map key reads back as nil). The
# store now owns its keys and values, so the entry survives its writer.
#
# Deterministic: the writer exits before the reader runs and the churn in between
# reuses the freed chunks. Tools use the interpreter's ETS namespace, so the table
# is created by a tool too.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# A process that writes through tools, then exits (freeing its region).
fun put_map(tid) { tool_call("stash", tid, "payload") ; send(self(), 'done') }
fun bump(tid) { tool_call("bump", tid, 5) ; send(self(), 'done') }

fun churn(n, acc) {
    if (n <= 0) { acc }
    else { churn(n - 1, acc + length("junk" ++ to_string(n))) }
}

fun main() {
    fails = 0
    tool_define("mk", "module T\nfun run() { ets_new() }")
    tool_define("stash", "module T\nfun run(t, tag) { ets_put(t, 7, %{\"tag\" => tag, \"body\" => tag ++ tag}); 'ok' }")
    tool_define("fetch", "module T\nfun run(t) { ets_get(t, 7) }")
    tool_define("bump", "module T\nfun run(t, d) { ets_update_counter(t, 9, d, 0) }")
    tool_define("count", "module T\nfun run(t) { ets_get(t, 9) }")

    tid = tool_call("mk")
    spawn(put_map(tid))
    receive { 'done' -> { 0 } after 5000 { 0 } }
    sleep(50)
    churn(2000, 0)
    fails = fails + assert_eq("tool_ets_put_survives_writer_exit",
        tool_call("fetch", tid), %{"tag" => "payload", "body" => "payloadpayload"})

    spawn(bump(tid))
    receive { 'done' -> { 0 } after 5000 { 0 } }
    sleep(50)
    churn(2000, 0)
    fails = fails + assert_eq("tool_ets_counter_survives_writer_exit",
        tool_call("count", tid), 5)

    if (fails > 0) { print("FAIL " ++ to_string(fails) ++ " assertion(s)"); sys_exit(1) }
    else { print("OK all tool ETS store assertions passed") }
}
