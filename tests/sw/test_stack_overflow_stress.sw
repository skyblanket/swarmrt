module Test_stack_overflow_stress

# Stress variant of test_stack_overflow.sw. The overflow EXIT reason must
# contain "stack overflow" on every probe, including when the scheduler is
# busy. Two things made it come back as a bare -1 under load:
#   - the parent's link() ran after the runaway child had already died, and
#     the dead-peer path read the panic message the child's teardown had
#     already freed;
#   - the child could die before the parent got to link() at all.
# So the probes alternate between linking straight away (racing the child)
# and linking only after the child is long gone (the late-link path). Six
# busy workers keep every scheduler thread loaded for the whole run.

fun spin(n) { if (n == 0) { 'done' } else { spin(n - 1) } }

fun spawn_spinners(n) {
    if (n == 0) { 'ok' }
    else { spawn(spin(30000000)) ; spawn_spinners(n - 1) }
}

# Non-tail recursion: mutual tail calls are optimized now, so a ping/pong
# tail cycle would loop forever instead of overflowing.
fun runaway_deep(n) { 1 + runaway_deep(n + 1) }
fun runaway() { runaway_deep(0) }

fun pause(ms) {
    receive {
        {'never_sent', x} -> x
        after ms { 'slept' }
    }
}

# Returns the number of probes whose EXIT reason lacks "stack overflow".
fun probe_rounds(i, bad) {
    if (i == 20) { bad }
    else {
        w = spawn(runaway())
        pause(if (i % 2 == 0) { 0 } else { 20 })
        link(w)
        got = receive {
            {'EXIT', _, reason} -> reason
            after 30000 { 'no_exit' }
        }
        if (string_contains(to_string(got), "stack overflow") == 'true') {
            probe_rounds(i + 1, bad)
        } else {
            print("FAIL overflow_stress round=" ++ to_string(i) ++ " got=" ++ to_string(got))
            probe_rounds(i + 1, bad + 1)
        }
    }
}

fun main() {
    trap_exit('true')
    spawn_spinners(6)
    bad = probe_rounds(0, 0)
    if (bad == 0) { print("OK test_stack_overflow_stress 20/20") ; sys_exit(0) }
    else { print(f"FAIL test_stack_overflow_stress: {bad}") ; sys_exit(1) }
}
