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

fun main() {
    fails = 0
    fails = fails + test_define_and_call()
    fails = fails + test_agent_composed_source()
    fails = fails + test_list()
    fails = fails + test_errors()
    fails = fails + test_redefine_and_rollback()
    fails = fails + test_unknown()
    if (fails == 0) { print("OK tool_registry 15/15") ; sys_exit(0) }
    else { print("FAIL tool_registry " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
