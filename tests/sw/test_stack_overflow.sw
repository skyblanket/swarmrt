module Test_stack_overflow

# Bidirectional gates for the 2026-07-05 swarm-code SIGBUS (fiber-stack
# overflow during a 154s tool wait). Two independent fixes, two probes:
#
# 1) after-arm TCO: emit_receive used to pass tail=0 into the `after` body,
#    so a heartbeat-shaped `receive {...} after N { loop(...) }` stacked a
#    real ~1.5KB C frame per tick and overflowed 128KB in ~80 iterations.
#    100k iterations here CRASH pre-fix (SIGBUS on the guard page) and run
#    flat post-fix.
#
# 2) red-zone recovery: a genuinely non-tail deep recursion (`1 + runaway()`)
#    must die as a NORMAL per-process panic — EXIT delivered to the linked,
#    trapping parent; OS process survives. Pre-fix the whole binary died by
#    native signal and no supervision layer ever ran. (Mutual tail calls are
#    optimized now, so a ping/pong tail cycle loops forever instead; see
#    test_mutual_tail.sw.)

fun tick_loop(n) {
    receive {
        {'never_sent', x} -> x
        after 0 {
            if (n > 0) { tick_loop(n - 1) }
            else { 'done' }
        }
    }
}

fun runaway(n) { 1 + runaway(n + 1) }

# Hold the overflow until the parent has linked us: a child that dies before
# link() reaches a reaped process, and the link then carries only the int reason.
fun runaway_on_go() {
    receive {
        'go' -> runaway(0)
    }
}

fun main() {
    failures = 0

    # --- 1. after-arm self-tail-call is TCO'd (flat stack over 100k ticks)
    r = tick_loop(100000)
    failures = failures + (if (r == 'done') { print("PASS after_arm_tco_flat") ; 0 }
                           else { print("FAIL after_arm_tco_flat") ; 1 })

    # --- 2. stack overflow is a recoverable actor panic, not process death
    trap_exit('true')
    w = spawn(runaway_on_go())
    link(w)
    send(w, 'go')
    got = receive {
        {'EXIT', _, reason} -> reason
        after 30000 { 'no_exit' }
    }
    ok_exit = if (got == 'no_exit') { 'false' }
              else { string_contains(to_string(got), "stack overflow") }
    failures = failures + (if (ok_exit == 'true') { print("PASS overflow_recovered_as_exit") ; 0 }
                           else { print("FAIL overflow_recovered_as_exit got=" ++ to_string(got)) ; 1 })

    if (failures == 0) { print("OK test_stack_overflow 2/2") ; sys_exit(0) }
    else { print(f"FAIL test_stack_overflow: {failures}") ; sys_exit(1) }
}
