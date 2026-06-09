# error_xproc_repro.sw — deterministic repro for the cross-process error()/try
# TLS-ownership bug (now fixed).
#
# THE BUG (pre-fix): generated `error(x)` / `try { } catch e { }` carried the
# raised value in a SCHEDULER-THREAD thread-local `_sw_error`. `error()` set it;
# `try` cleared it at entry and re-read it after the body. But a process running
#     try { sleep(N); <body-value> } catch e { ... }
# BLOCKS in sleep() — sleep compiles to a C-level sw_receive_any that
# context-switches fibers. While that process is PARKED, ANOTHER process on the
# SAME scheduler thread calls error("contaminant"), which writes the SHARED
# thread-local. The parked process resumes, its `try` checks `_sw_error`, finds
# the foreign value, and WRONGLY CATCHES another process's error. Worse: that
# error value lives in the OTHER process's arena and is freed when that process
# exits -> use-after-free (ASAN flags it; the value is deep-copied in the catch).
#
# THE FIX: `_sw_error` is a PER-PROCESS slot (sw_self_error_slot ->
# &tls_current->gen_error), not a thread-local. A `try` resuming after a blocking
# op reads ITS OWN slot, which it cleared at entry and which no other fiber can
# touch — so it observes NO error and returns its body.
#
# DETERMINISTIC INTERLEAVE (run under SW_SCHEDULERS=1 — one OS thread, one runq,
# cooperative fibers; verified with a scheduling probe that spinners run MANY
# times while a long-sleeping victim is parked):
#   * `victim` runs `try { sleep(VICTIM_SLEEP); MARKER } catch e { ... }`. The
#     codegen clears its error slot, then sleep() PARKS it for a long window.
#   * Many `contaminant` processes loop: error("CONTAMINANT") ; sleep(SHORT).
#     Each error() re-sets the SHARED thread-local (pre-fix). Because each
#     contaminant's loop FINISHES (and the process EXITS, freeing its arena)
#     well BEFORE the victim wakes, the shared slot pre-fix holds a pointer into
#     an ALREADY-FREED arena when the victim resumes -> wrong catch AND UAF.
#   * The victim wakes, its `try` checks the slot.
#       - FIXED build: own slot is still NULL -> no catch -> returns MARKER.
#       - PRE-FIX build: slot holds the foreign (freed) "CONTAMINANT..." string
#         -> wrong catch (logic bug) and a deep_copy of a freed arena value (UAF
#         under ASAN + -DSW_ARENA_POISON).
#
# The victim ASSERTS it got MARKER and did NOT catch. PROBE_OK + exit 0 on the
# fix; exit 1 (or ASAN fault) on the bug.

module Main

# A distinctive heap-allocated reason string the contaminants raise. If the
# victim ever sees this, it caught a FOREIGN error.
fun contaminant_reason(id) {
    "CONTAMINANT-" ++ to_string(id) ++ "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
}

# Contaminant: raise an error and yield, repeatedly, then EXIT (freeing this
# process's arena). error() writes the shared thread-local pre-fix. The short
# sleeps yield to the scheduler so a victim parked on the same OS thread keeps
# seeing a freshly-set (then freed) foreign error.
fun contaminate(id, rounds) {
    if (rounds <= 0) { 0 }
    else {
        error(contaminant_reason(id))   # sets the (shared, pre-fix) slot
        sleep(3)                        # yield; victim stays parked
        contaminate(id, rounds - 1)
    }
}

fun spawn_contaminants(n) {
    if (n <= 0) { 0 }
    else {
        # Each does ~8 rounds * (3ms sleep) ~= 24ms of activity, far less than
        # the victim's parked window, so it exits (arena freed) while the victim
        # is still parked.
        spawn(contaminate(n, 8))
        spawn_contaminants(n - 1)
    }
}

# The victim. Returns a tuple {result, caught_flag, caught_value}:
#   FIXED:   {'MARKER', 0, 'none'}      — body ran, no catch.
#   PRE-FIX: {'CAUGHT', 1, <foreign>}   — wrongly caught another process's error.
fun victim_try() {
    try {
        sleep(300)            # PARK here; contaminants run + exit meanwhile
        'MARKER'              # body value — must be what the fix returns
    } catch e {
        # If we get here at all under this interleave it is WRONG: nothing in
        # this process raised. e is a FOREIGN error value living in a
        # contaminant's arena that has ALREADY EXITED + been freed (poisoned
        # under -DSW_ARENA_POISON). READING it here (to_string deep-walks the
        # string bytes) is the use-after-free the fix prevents. On the fixed
        # build this branch is never reached, so the read never happens.
        'CAUGHT-' ++ to_string(e)
    }
}

fun victim(coord) {
    r = victim_try()
    if (r == 'MARKER') {
        send(coord, {'result', 'ok'})
    } else {
        # r == 'CAUGHT' -> the try caught a foreign error: the cross-process bug.
        send(coord, {'result', 'wrongcatch'})
    }
}

fun await(coord) {
    receive {
        {'result', verdict} -> verdict
        after 20000 { 'timeout' }
    }
}

fun main() {
    print("error_xproc_repro start (run with SW_SCHEDULERS=1)")
    me = self()
    # Spawn the victim FIRST so it is parked in sleep() before the contaminants
    # start hammering error().
    spawn(victim(me))
    sleep(20)                  # let the victim reach its sleep() and park
    spawn_contaminants(12)     # 12 processes hammering error() then exiting
    verdict = await(me)
    if (verdict == 'ok') {
        print("PROBE_OK victim returned its body, caught NO foreign error")
        sys_exit(0)
    } else {
        print("PROBE_FAIL victim verdict=" ++ to_string(verdict) ++
              " (wrong cross-process catch)")
        sys_exit(1)
    }
}
