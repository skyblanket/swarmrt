# ets_outlive_repro.sw — an ETS entry written through a tool must outlive the
# writer. The writer's per-call tool region is adopted by the writer process and
# freed when it exits, so the ETS store must own its value (a raw pointer into
# that region reads back as garbage once the region is reused).
#
# DETERMINISTIC: the writer exits, churn reuses the freed memory, then the entry
# is read back through a second tool. Expected output: PROBE_OK.
module Main

fun churn_junk(n, acc) {
    if (n <= 0) { acc }
    else { churn_junk(n - 1, acc + length("junk" ++ to_string(n))) }
}

# A process that writes through a tool, then exits (freeing its region).
fun put_entry(tid) { tool_call("stash", tid, "payload") ; send(self(), 'done') }

# The same, with a closure that captures the writer's per-call value.
fun put_fn(tid, tag, parent) { tool_call("stash_fn", tid, tag) ; send(parent, 'done') }

fun warm(calls, acc) {
    if (calls <= 0) { acc }
    else { warm(calls - 1, acc + tool_call("churn", calls, 3)) }
}

fun main() {
    tool_define("churn", "module T\nfun run(i, k) { s = \"0123456789abcdef\" ++ to_string(i); l = [s, s ++ s, s ++ s ++ s]; length(l) + k }")
    tool_define("mk", "module T\nfun run() { ets_new() }")
    tool_define("stash", "module T\nfun run(t, tag) { ets_put(t, 7, %{\"tag\" => tag, \"body\" => tag ++ tag}); 'ok' }")
    tool_define("fetch", "module T\nfun run(t) { ets_get(t, 7) }")
    tool_define("stash_fn", "module T\nfun run(t, tag) { k = tag ++ \"-\"; ets_put(t, 1, fn(x) { x ++ k }); 'ok' }")
    tool_define("call_fn", "module T\nfun run(t, a) { g = ets_get(t, 1); g(a) }")
    warm(2000, 0)
    tid = tool_call("mk")
    spawn(put_entry(tid))
    receive { 'done' -> { 0 } after 5000 { 0 } }
    sleep(50)
    churn_junk(2000, 0)
    got = tool_call("fetch", tid)
    if (got != %{"tag" => "payload", "body" => "payloadpayload"}) {
        print("FAIL ets entry lost its writer's value: " ++ to_string(got)) ; sys_exit(1)
    }
    fid = tool_call("mk")
    me = self()
    spawn(put_fn(fid, "K", me))
    receive { 'done' -> { 0 } after 5000 { 0 } }
    sleep(50)
    churn_junk(2000, 0)
    called = tool_call("call_fn", fid, "x")
    if (called == "xK-") { print("PROBE_OK") }
    else { print("FAIL ets closure lost its writer's env: " ++ to_string(called)) ; sys_exit(1) }
}
