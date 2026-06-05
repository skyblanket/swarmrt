module Test_tool_registry

# Tool registry — an agent writes a new tool AS sw source at runtime and calls
# it live, no restart (lib-free builtins: tool_define / tool_call / tool_list /
# tool_rollback). Compiled-only: tool eval runs the embedded interpreter on a
# process fiber, so this runs only on the compile-then-execute path, like
# test_dynsup.sw. (Under swc run the tool_* builtins warn + return nil.)

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# 'true' if r is an {'error', reason} tuple.
fun is_err(r) {
    case r {
        {'error', _} -> 'true'
        _ -> 'false'
    }
}

# 'true' if tool_list() contains a tool named `name`.
fun has_tool(name) { _ht(tool_list(), name) }
fun _ht(lst, name) {
    if (length(lst) == 0) { 'false' }
    else {
        if (elem(hd(lst), 0) == name) { 'true' }
        else { _ht(tl(lst), name) }
    }
}

# Define tools from source, call them (numeric + string). The headline.
fun test_define_and_call() {
    fails = 0
    fails = fails + assert_eq("adder_define", tool_define("adder", "module T\nfun run(a, b) { a + b }"), 'ok')
    fails = fails + assert_eq("adder_call", tool_call("adder", 40, 2), 42)
    tool_define("greet", "module T\nfun run(name) { \"hi \" ++ name }")
    fails = fails + assert_eq("greet_call", tool_call("greet", "sky"), "hi sky")
    fails
}

# The agent COMPOSES the source string at runtime, then defines + calls it.
fun test_agent_composed_source() {
    factor = 3
    src = "module T\nfun run(x) { x * " ++ to_string(factor) ++ " }"
    fails = assert_eq("composed_define", tool_define("scale", src), 'ok')
    fails = fails + assert_eq("composed_call", tool_call("scale", 7), 21)
    fails
}

# tool_list reflects what's registered.
fun test_list() {
    fails = assert_eq("has_adder", has_tool("adder"), 'true')
    fails = fails + assert_eq("no_ghost", has_tool("ghost_tool"), 'false')
    fails
}

# Bad input fails LOUDLY with {'error', reason} — never a silent nil.
fun test_errors() {
    fails = assert_eq("bad_parse_is_error", is_err(tool_define("bad", "this is not sw {{{")), 'true')
    fails = fails + assert_eq("no_run_is_error", is_err(tool_define("norun", "module T\nfun nope() { 1 }")), 'true')
    fails = fails + assert_eq("good_define_not_error", is_err(tool_define("ok_tool", "module T\nfun run() { 1 }")), 'false')
    fails
}

# Redefine swaps behavior live; rollback reverts to the previous version.
fun test_redefine_and_rollback() {
    tool_define("calc", "module T\nfun run(a, b) { a + b }")
    fails = assert_eq("calc_v1", tool_call("calc", 1, 1), 2)
    tool_define("calc", "module T\nfun run(a, b) { (a + b) * 100 }")
    fails = fails + assert_eq("calc_v2", tool_call("calc", 1, 1), 200)
    fails = fails + assert_eq("rollback_ok", tool_rollback("calc"), 'ok')
    fails = fails + assert_eq("calc_after_rollback", tool_call("calc", 1, 1), 2)
    fails
}

# Calling an unknown tool degrades to nil (not a crash).
fun test_unknown() {
    assert_eq("unknown_tool_nil", tool_call("does_not_exist", 1), nil)
}

# A tool's run can call helpers, recurse, and use lambdas/HOFs. (Regression:
# these all silently returned nil because the recursion guard measured the wrong
# stack when eval ran on the process fiber.)
fun test_helper_recursion_lambda() {
    tool_define("wh", "module T\nfun h(x) { x * 10 }\nfun run(x) { h(x) }")
    tool_define("rec", "module T\nfun run(n) { if (n == 0) { 0 } else { n + run(n - 1) } }")
    tool_define("lam", "module T\nfun run() { f = fun(x) { x + 1 }\nf(10) }")
    tool_define("mp", "module T\nfun run(xs) { map(xs, fn(x) { x * 2 }) }")
    fails = 0
    fails = fails + assert_eq("helper_call", tool_call("wh", 5), 50)
    fails = fails + assert_eq("self_recursion", tool_call("rec", 5), 15)
    fails = fails + assert_eq("lambda", tool_call("lam"), 11)
    fails = fails + assert_eq("map_hof", tool_call("mp", [1, 2, 3]), [2, 4, 6])
    fails
}

# A faulting call must NOT permanently poison the tool. (Regression: interp->error
# was sticky across the retained interp; fresh per-call interp fixes it.)
fun test_no_poison() {
    tool_define("mx", "module T\nfun run(x) { if (x == 0) { 100 } else { hd([]) } }")
    fails = assert_eq("poison_before", tool_call("mx", 0), 100)
    tool_call("mx", 1)                                  # faults (hd of empty)
    fails = fails + assert_eq("poison_after", tool_call("mx", 0), 100)
    fails
}

# A deeply-nested expression must not crash the host (clean value, not SIGBUS).
# (Assert via a count-independent check so the test can't drift on term count.)
fun test_deep_expr_survives() {
    tool_define("deep", "module T\nfun run(n) { s = 1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1\nif (s > 0) { 'deep_ok' } else { 'deep_bad' } }")
    assert_eq("deep_expr_survives", tool_call("deep", 0), 'deep_ok')
}

# Process primitives inside a tool degrade (no crash, no hang) — sw_self() is
# nil on the tool's eval thread, so send returns and receive doesn't block.
fun test_proc_prims_degrade() {
    tool_define("rcv", "module T\nfun run() { receive { x -> x } }")
    assert_eq("receive_degrades", tool_call("rcv"), nil)
}

# Over-length names are rejected loudly, not silently truncated into name[64]
# (a truncated name never matches on tool_call — the BUG-6 trap).
fun test_name_validation() {
    longnm = "fetch_and_summarize_customer_support_tickets_from_the_last_30_days_x"
    assert_eq("long_name_err", is_err(tool_define(longnm, "module T\nfun run() { 1 }")), 'true')
}

# Concurrency: many fibers calling the same tool must not crash or corrupt
# (each tool_call gets its own isolated interpreter). mismatches must be 0.
fun cc_worker(parent, i, hi, bad) {
    if (i >= hi) { send(parent, {'done', bad}) }
    else {
        r = tool_call("idfn", i)
        nb = if (r == i) { bad } else { bad + 1 }
        cc_worker(parent, i + 1, hi, nb)
    }
}
fun cc_collect(n, bad) {
    if (n == 0) { bad }
    else { receive { {'done', b} -> cc_collect(n - 1, bad + b) after 15000 { 999 } } }
}
fun test_concurrent_calls() {
    tool_define("idfn", "module T\nfun run(x) { x }")
    me = self()
    spawn(fun() { cc_worker(me, 0, 1500, 0) })
    spawn(fun() { cc_worker(me, 0, 1500, 0) })
    spawn(fun() { cc_worker(me, 0, 1500, 0) })
    spawn(fun() { cc_worker(me, 0, 1500, 0) })
    assert_eq("concurrent_no_corruption", cc_collect(4, 0), 0)
}

fun main() {
    fails = 0
    fails = fails + test_define_and_call()
    fails = fails + test_agent_composed_source()
    fails = fails + test_list()
    fails = fails + test_errors()
    fails = fails + test_redefine_and_rollback()
    fails = fails + test_unknown()
    fails = fails + test_helper_recursion_lambda()
    fails = fails + test_no_poison()
    fails = fails + test_deep_expr_survives()
    fails = fails + test_proc_prims_degrade()
    fails = fails + test_name_validation()
    fails = fails + test_concurrent_calls()
    if (fails == 0) { print("OK tool_registry 25/25") ; sys_exit(0) }
    else { print("FAIL tool_registry " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
