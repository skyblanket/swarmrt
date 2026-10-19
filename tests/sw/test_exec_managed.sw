module Test_exec_managed

# exec_managed(cmd, args, opts): exec_argv (no shell) plus timeout with
# process-group kill, cwd, stdin / stdin_file, stdout_file / stderr_file and
# env_unset.

import Std

fun check(name, cond) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name) ; 1 }
}

fun yes(b) { if (b) { 'true' } else { 'false' } }

fun main() {
    f = 0
    dir = "/tmp/sw_exec_managed_t"
    shell("rm -rf " ++ dir ++ " && mkdir -p " ++ dir ++ "/sub")

    # defaults behave like exec_argv
    r = exec_managed("echo", ["hi"], %{})
    f = f + check("defaults", yes(elem(r, 0) == 0 && elem(r, 1) == "hi\n"))
    r = exec_managed("sh", ["-c", "exit 3"])
    f = f + check("exit_code", yes(elem(r, 0) == 3))

    # timeout kills the whole group, including a backgrounded grandchild
    pidf = dir ++ "/gc.pid"
    t0 = timestamp()
    r = exec_managed("sh", ["-c", "sleep 30 & echo $! > " ++ pidf ++ "; wait"], %{timeout_ms: 500})
    el = timestamp() - t0
    f = f + check("timeout_124", yes(elem(r, 0) == 124 && el < 5000))
    gc = string_trim(file_read(pidf))
    sleep(100)
    # an orphan nobody reaps (no init in a container) lingers as a zombie: only
    # a live, non-zombie process counts as having survived
    # (`ps` rather than /proc so this also checks on macOS)
    alive = shell("st=$(ps -o stat= -p " ++ gc ++ " 2>/dev/null); case \"$st\" in \"\"|Z*) echo dead;; *) echo alive;; esac")
    f = f + check("timeout_kills_grandchild", yes(string_trim(elem(alive, 1)) == "dead"))

    # a command that finishes inside the timeout is untouched
    r = exec_managed("echo", ["fast"], %{timeout_ms: 5000})
    f = f + check("timeout_not_hit", yes(elem(r, 0) == 0 && elem(r, 1) == "fast\n"))

    # cwd
    # (reads a relative file rather than comparing `pwd`: macOS /tmp is a
    # symlink, so pwd prints the resolved path)
    file_write(dir ++ "/sub/marker.txt", "in sub")
    r = exec_managed("cat", ["marker.txt"], %{cwd: dir ++ "/sub"})
    f = f + check("cwd", yes(elem(r, 0) == 0 && elem(r, 1) == "in sub"))

    # stdin_file and stdin
    file_write(dir ++ "/in.txt", "from file")
    r = exec_managed("cat", [], %{stdin_file: dir ++ "/in.txt"})
    f = f + check("stdin_file", yes(elem(r, 0) == 0 && elem(r, 1) == "from file"))
    r = exec_managed("cat", [], %{stdin: "from string"})
    f = f + check("stdin_string", yes(elem(r, 0) == 0 && elem(r, 1) == "from string"))

    # redirects
    r = exec_managed("sh", ["-c", "echo out; echo err >&2"],
                     %{stdout_file: dir ++ "/o.txt", stderr_file: dir ++ "/e.txt"})
    f = f + check("redirect_return", yes(elem(r, 0) == 0 && elem(r, 1) == ""))
    f = f + check("stdout_file", yes(file_read(dir ++ "/o.txt") == "out\n"))
    f = f + check("stderr_file", yes(file_read(dir ++ "/e.txt") == "err\n"))

    # an argument with spaces, quotes and $(...) arrives verbatim
    nasty = "a b 'c' \"d\" $(echo pwned) `x` ; | &"
    r = exec_managed("printf", ["%s", nasty], %{})
    f = f + check("argv_verbatim", yes(elem(r, 1) == nasty))

    # env_unset
    r = exec_managed("sh", ["-c", "echo \"[$HOME]\""], %{env_unset: ["HOME"]})
    f = f + check("env_unset", yes(elem(r, 1) == "[]\n"))

    # env_unset leaves the rest of the environment alone
    r = exec_managed("sh", ["-c", "echo \"[$PATH]\""], %{env_unset: ["HOME"]})
    f = f + check("env_unset_keeps_others", yes(elem(r, 1) != "[]\n"))

    # a killed caller takes its child with it: a task_stream deadline kills the
    # worker while it waits in exec_managed (no timeout_ms), and the child must
    # not outlive it. (Compiled only, as this suite runs: `swc run` is
    # single-scheduler, so nothing can kill a caller mid-call there.)
    rs = Std.task_stream(["x"], fn(t) { exec_managed("sleep", ["7.25"], %{}) },
                         %{max_concurrency: 1, timeout_ms: 300})
    sleep(500)
    left = shell("pgrep -f 'sleep [7][.]25' >/dev/null && echo running || echo gone")
    f = f + check("killed_caller_kills_child", yes(hd(rs) == {'error', 'timeout'} && string_trim(elem(left, 1)) == "gone"))
    shell("pkill -f 'sleep [7][.]25'")

    shell("rm -rf " ++ dir)
    if (f == 0) { print("OK exec_managed 15/15") ; sys_exit(0) } else { print("FAIL exec_managed") ; sys_exit(1) }
}
