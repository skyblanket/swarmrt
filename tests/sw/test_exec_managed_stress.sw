module Test_exec_managed_stress

# exec_managed with env_unset, many calls at once. The child builds its
# environment in the parent and only makes async-signal-safe calls before
# exec, so none of these may hang or lose its output.

import Std

fun check(name, cond) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name) ; 1 }
}

fun yes(b) { if (b) { 'true' } else { 'false' } }

fun good(r) {
    case r {
        {'ok', v} -> elem(v, 0) == 0 && elem(v, 1) != ""
        _ -> 'false'
    }
}

fun main() {
    f = 0
    rs = Std.task_stream(Std.range(0, 400),
        fn(i) { exec_managed("sh", ["-c", "echo \"[$HOME]\"", "x"], %{env_unset: ["HOME", "PATH"]}) },
        %{max_concurrency: 16, timeout_ms: 20000})
    bad = Std.filter(rs, fn(r) { good(r) == 'false' })
    f = f + check("concurrent_env_unset", yes(length(bad) == 0 && length(rs) == 400))

    r = exec_managed("no_such_program_xyz", [], %{env_unset: ["PATH"]})
    f = f + check("missing_program_127", yes(elem(r, 0) == 127))

    if (f == 0) { print("OK exec_managed_stress 2/2") ; sys_exit(0) }
    else { print("FAIL exec_managed_stress " ++ to_string(f) ++ " failed") ; sys_exit(1) }
}
