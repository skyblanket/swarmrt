# Sandbox.sw — a Docker container as the sandbox for one agent episode.
# Pure sw over exec_managed / shell / pid_alive; the Docker CLI does the work.
#
#   import Sandbox
#   sb = Sandbox.start("python:3.12-slim", %{network: "none"})
#   r  = Sandbox.exec(sb, "sh", ["-c", "pytest -q"], %{timeout_ms: 60000})
#   # elem(r, 0) = exit code (124 = timed out), elem(r, 1) = stdout
#   Sandbox.stop(sb)
#
# Lifecycle. Each container gets the labels swarmrt.sandbox=1 and
# swarmrt.owner=<OS pid of the runtime that started it>, and a --cidfile. A
# detached watcher polls the owner pid every 0.5 s and removes the container
# once the owner is gone, so killing the owner (SIGKILL included) reaps its
# container within about a second. stop() ends the watcher by deleting the
# cidfile. start() first sweeps: containers whose owner pid is dead (a crashed
# run's leftovers) are removed; containers of a live run, or without the label,
# are never touched.
#
# exec() runs the command as its own process group inside the container and
# records that group id. On timeout, exec() kills the group inside the
# container, not only the docker client, so nothing the command started
# survives it. Option keys are atoms. start opts: network, memory, cpus,
# workdir. exec opts: exec_managed's (timeout_ms, stdin, stdin_file, ...) plus
# cwd, the working directory inside the container.
#
# Needs a docker CLI and daemon. Inside the image, exec() needs sh, kill and
# util-linux setsid (`setsid -w`), which Debian and Ubuntu images ship. On the
# host, setsid is optional: without it the watcher still runs, just attached to
# the owner's process group.

module Sandbox

export [start, exec, stop, sweep]

# start(image, opts) → sandbox map, or nil when docker run fails. The map is
# plain data: %{id, owner, cidfile}.
fun start(image, opts) {
    sweep()
    owner = owner_pid()
    cidfile = tmp_dir() ++ "/sw-sandbox-" ++ uuid() ++ ".cid"
    args = ["run", "-d", "--label", label() ++ "=1", "--label", owner_key() ++ "=" ++ owner,
            "--cidfile", cidfile] ++ run_flags(opts) ++ [image, "sleep", "infinity"]
    r = run_docker(args, %{timeout_ms: 600000})
    if (elem(r, 0) != 0) {
        shell(reap_script(cidfile))
        nil
    } else {
        watch(cidfile, owner)
        %{id: string_trim(elem(r, 1)), owner: owner, cidfile: cidfile}
    }
}

# exec(sb, cmd, args, opts) → {exit_code, stdout}, as exec_managed returns.
# The command runs in its own process group inside the container. On timeout
# (exit 124) that group is SIGKILLed inside the container as well.
fun exec(sb, cmd, args, opts) {
    pidf = "/tmp/sw-sandbox-" ++ uuid() ++ ".pgid"
    # `setsid -w` starts the command in a new session, so its pid is also its
    # process group id (`set -m` does not work without a tty). The wrapper
    # records that id, then execs the command, keeping its pid.
    wrap = "echo $$ > \"$0\"; exec \"$@\""
    cwd = map_get(opts, 'cwd', nil)
    stdin = map_get(opts, 'stdin', nil) != nil || map_get(opts, 'stdin_file', nil) != nil
    in_flags = if (stdin) { ["-i"] } else { [] }
    cwd_flags = if (cwd == nil) { [] } else { ["--workdir", to_string(cwd)] }
    dargs = ["exec"] ++ in_flags ++ cwd_flags ++
        [map_get(sb, 'id'), "setsid", "-w", "sh", "-c", wrap, pidf, cmd] ++ args
    r = run_docker(dargs, map_remove(opts, 'cwd'))
    if (elem(r, 0) == 124) { kill_group(sb, pidf) }
    r
}

# stop(sb) → 'ok'. Ends the watcher (it exits once the cidfile is gone) and
# removes the container.
fun stop(sb) {
    file_delete(map_get(sb, 'cidfile'))
    run_docker(["rm", "-f", map_get(sb, 'id')], %{timeout_ms: 60000})
    'ok'
}

# sweep() → number of containers removed. Removes this module's containers
# whose owner runtime is dead. A container with no owner label is left alone.
fun sweep() {
    r = run_docker(["ps", "-a", "--filter", "label=" ++ label(), "--format", "{{.ID}} {{.Labels}}"],
                   %{timeout_ms: 30000})
    if (elem(r, 0) != 0) { 0 }
    else { reap_orphans(string_split(elem(r, 1), "\n")) }
}

# ---- helpers ----

fun label() { "swarmrt.sandbox" }
fun owner_key() { "swarmrt.owner" }

# The runtime's OS pid. shell() forks sh directly from the runtime, so the
# shell's $PPID is that pid.
fun owner_pid() { string_trim(elem(shell("echo $PPID"), 1)) }

fun tmp_dir() {
    t = getenv("TMPDIR")
    if (t == nil || t == "") { "/tmp" } else { t }
}

# Run the docker CLI through exec_managed. A failed fork gives {-1, ""}, so
# callers can always destructure the result.
fun run_docker(args, opts) {
    case exec_managed("docker", args, opts) { nil -> {-1, ""} ; r -> r }
}

fun run_flags(opts) {
    docker_flag("--network", map_get(opts, 'network', nil)) ++
    docker_flag("--memory", map_get(opts, 'memory', nil)) ++
    docker_flag("--cpus", map_get(opts, 'cpus', nil)) ++
    docker_flag("--workdir", map_get(opts, 'workdir', nil))
}

fun docker_flag(name, v) { if (v == nil) { [] } else { [name, to_string(v)] } }

# POSIX single-quote a string for the shell.
fun q(s) { "'" ++ string_replace(s, "'", "'\\''") ++ "'" }

# Removes the container named in cidfile, then the cidfile. A no-op once the
# cidfile is gone.
fun reap_script(cidfile) {
    "if [ -e " ++ q(cidfile) ++ " ]; then docker rm -f \"$(cat " ++ q(cidfile) ++
        ")\" >/dev/null 2>&1; rm -f " ++ q(cidfile) ++ "; fi"
}

# Detached watcher: poll while the owner lives and the cidfile exists, then
# reap. setsid puts it in its own session, so the owner's process group being
# killed does not take the watcher down with it.
fun watch(cidfile, owner) {
    loop = "while [ -e " ++ q(cidfile) ++ " ] && kill -0 " ++ owner ++
        " 2>/dev/null; do sleep 0.5; done; " ++ reap_script(cidfile)
    shell("S=$(command -v setsid); $S sh -c " ++ q(loop) ++ " </dev/null >/dev/null 2>&1 &")
    'ok'
}

# SIGKILL the process group recorded in pidf, inside the container. Only a
# group id above 1 is trusted: kill -- -1 would signal every process.
fun kill_group(sb, pidf) {
    s = "p=$(cat " ++ q(pidf) ++ " 2>/dev/null); if [ \"${p:-0}\" -gt 1 ]; then kill -s KILL -- -\"$p\"; fi"
    run_docker(["exec", map_get(sb, 'id'), "sh", "-c", s], %{timeout_ms: 10000})
    'ok'
}

fun reap_orphans(lines) {
    if (length(lines) == 0) { 0 }
    else { reap_line(string_trim(hd(lines))) + reap_orphans(tl(lines)) }
}

# One `docker ps` line, "<id> <labels>". Removes it only if its owner is dead.
fun reap_line(line) {
    parts = string_split(line, " ")
    if (length(parts) < 2) { 0 }
    else {
        owner = label_value(hd(tl(parts)), owner_key())
        if (owner == "" || pid_alive(owner) == 'true') { 0 }
        else { run_docker(["rm", "-f", hd(parts)], %{timeout_ms: 60000}) ; 1 }
    }
}

# Value of key in a docker labels string ("k=v,k2=v2"), or "" when absent.
fun label_value(labels, key) {
    hits = filter(fun(kv) { string_starts_with(kv, key ++ "=") }, string_split(labels, ","))
    if (length(hits) == 0) { "" } else { string_replace(hd(hits), key ++ "=", "") }
}
