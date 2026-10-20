module Test_sandbox

# Sandbox (lib/Sandbox.sw): a container dies with the process that started it,
# an exec timeout kills what the command started inside the container, and
# the sweeper removes only the containers of dead owners. Needs a docker daemon
# and the image named by SW_SANDBOX_TEST_IMAGE (default debian:bookworm-slim); without
# either the test passes as skipped.

import Std
import Sandbox

fun check(name, cond) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name) ; 1 }
}

fun yes(b) { if (b) { 'true' } else { 'false' } }

fun image() {
    img = getenv("SW_SANDBOX_TEST_IMAGE")
    if (img == nil || img == "") { "debian:bookworm-slim" } else { img }
}

fun docker(args) { exec_managed("docker", args, %{timeout_ms: 60000}) }
fun docker_out(args) { string_trim(elem(docker(args), 1)) }

# The container is still registered with the daemon (running or not).
fun present(id) { docker_out(["ps", "-aq", "--filter", "id=" ++ id]) != "" }

fun my_pid() { string_trim(elem(shell("echo $PPID"), 1)) }

# Run a container labelled with extra_labels, outside Sandbox.
fun run_container(extra) {
    docker_out(["run", "-d"] ++ extra ++ [image(), "sleep", "infinity"])
}

fun wait_until_gone(id, tries) {
    if (present(id) == 'false') { 'true' }
    else { if (tries <= 0) { 'false' } else { sleep(100) ; wait_until_gone(id, tries - 1) } }
}

fun wait_for_file(path, tries) {
    if (file_exists(path) == 'true') { 'true' }
    else { if (tries <= 0) { 'false' } else { sleep(100) ; wait_for_file(path, tries - 1) } }
}

# Child mode: start a sandbox, publish "<id> <owner>", then wait to be killed.
fun owner_main(out) {
    sb = Sandbox.start(image(), %{})
    file_write(out, map_get(sb, 'id') ++ " " ++ to_string(map_get(sb, 'owner')))
    sleep(600000)
}

fun test_exec_basics(f0) {
    sb = Sandbox.start(image(), %{})
    r = Sandbox.exec(sb, "sh", ["-c", "echo hi; exit 3"], %{})
    f = f0 + check("exec_exit_and_stdout", yes(elem(r, 0) == 3 && elem(r, 1) == "hi\n"))
    r = Sandbox.exec(sb, "sh", ["-c", "cat"], %{stdin: "fed"})
    f = f + check("exec_stdin", yes(elem(r, 1) == "fed"))
    Sandbox.stop(sb)
    f = f + check("stop_removes_container", yes(present(map_get(sb, 'id')) == 'false'))
    f
}

# exec times out; the backgrounded and foreground sleeps must both be gone
# from inside the container, not only the docker client.
fun test_timeout_kills_inside(f0) {
    sb = Sandbox.start(image(), %{})
    r = Sandbox.exec(sb, "sh", ["-c", "sleep 300 & sleep 300"], %{timeout_ms: 1000})
    sleep(300)
    top = docker_out(["top", map_get(sb, 'id')])
    f = f0 + check("timeout_exit_124", yes(elem(r, 0) == 124))
    f = f + check("timeout_leaves_no_process", yes(string_contains(top, "sleep 300") == 'false'))
    Sandbox.stop(sb)
    f
}

# Killing the owner removes its container within the watcher's poll window.
fun test_owner_death(f0, bin) {
    out = tmp_base() ++ "/sw_sandbox_owner_" ++ uuid() ++ ".txt"
    shell(bin ++ " owner " ++ out ++ " >/dev/null 2>&1 &")
    f = f0 + check("owner_child_started", yes(wait_for_file(out, 100) == 'true'))
    parts = string_split(string_trim(file_read(out)), " ")
    id = hd(parts)
    pid = hd(tl(parts))
    f = f + check("owner_container_running", yes(present(id) == 'true'))
    shell("kill -9 " ++ pid)
    f = f + check("owner_kill_removes_container", yes(wait_until_gone(id, 30) == 'true'))
    f
}

# The sweeper removes a dead owner's container, and leaves a live owner's and
# an unlabelled one alone.
fun test_sweep(f0) {
    dead = string_trim(elem(shell("sh -c 'echo $$'"), 1))
    orphan = run_container(["--label", "swarmrt.sandbox=1", "--label", "swarmrt.owner=" ++ dead])
    live = run_container(["--label", "swarmrt.sandbox=1", "--label", "swarmrt.owner=" ++ my_pid()])
    bare = run_container([])
    removed = Sandbox.sweep()
    f = f0 + check("sweep_counts_one", yes(removed == 1))
    f = f + check("sweep_removes_dead_owner", yes(present(orphan) == 'false'))
    f = f + check("sweep_keeps_live_owner", yes(present(live) == 'true'))
    f = f + check("sweep_keeps_unlabelled", yes(present(bare) == 'true'))
    docker(["rm", "-f", orphan, live, bare])
    f
}

fun tmp_base() {
    t = getenv("TMPDIR")
    if (t == nil || t == "") { "/tmp" } else { t }
}

fun main() {
    args = os_args()
    if (length(args) > 2 && hd(tl(args)) == "owner") { owner_main(hd(tl(tl(args)))) ; sys_exit(0) }
    if (elem(shell("docker info >/dev/null 2>&1"), 0) != 0 ||
        elem(shell("docker image inspect " ++ image() ++ " >/dev/null 2>&1"), 0) != 0) {
        print("SKIP sandbox: no docker daemon or image " ++ image())
        print("OK sandbox skipped")
        sys_exit(0)
    }
    f = 0
    f = test_exec_basics(f)
    f = test_timeout_kills_inside(f)
    f = test_owner_death(f, hd(args))
    f = test_sweep(f)
    if (f == 0) { print("OK sandbox") ; sys_exit(0) } else { print("FAIL sandbox") ; sys_exit(1) }
}
