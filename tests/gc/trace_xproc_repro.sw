# trace_xproc_repro.sw — deterministic repro for the cross-process panic-TRACE
# contamination bug (now fixed). Companion to error_xproc_repro.sw.
#
# THE BUG (pre-fix): the generated panic line/file (`_sw_current_line`,
# `_sw_current_file`) and the call-stack ring buffer (`_sw_trace`, `_sw_trace_top`)
# were SCHEDULER-THREAD thread-locals. The call depth (`_sw_trace_top`) is shared
# across every fiber on the OS thread, so when a process is parked inside a
# blocking op (sleep/receive) at some depth and the scheduler runs ANOTHER process
# on the same thread, that process pushes its frames from the parked process's
# depth and (if it also parks) leaves the shared top elevated. A third process that
# then panics inherits the foreign frames below its own — its panic call chain
# lists functions from an UNRELATED process. Memory-safe (the frames hold only
# .rodata function-name literals + ints), but the diagnostic is WRONG: a panic in
# process A blames process B's call stack.
#
# THE FIX: line/file/trace live on the CARRIER a process runs on (`sw_gen_exec_t`
# in swarmrt_native.h, reached through `_sw_gen`, pointed at the carrier on every
# context switch; Phase 2g). Every unpinned run starts with an empty ring, a
# process that blocks the old way keeps its carrier and so its ring, and a
# stackless process keeps its line in sw_process_t while parked. A panic prints
# the ring plus the process's heap frames, so the chain is the panicking
# process's own and complete even after it parked and resumed.
#
# DETERMINISTIC INTERLEAVE (run under SW_SCHEDULERS=1 — one OS thread, one runq):
#   * A `contaminant` builds a DEEP, distinctively-named chain (xcontam_a..e) and
#     parks ~forever in sleep(). Pre-fix this leaves the shared trace_top high with
#     xcontam_* frames in the buffer.
#   * A `victim` then builds a SHORT, distinctively-named chain (xvictim_a..b) and
#     panics. Pre-fix it pushed its frames ON TOP of the contaminant's parked
#     frames (shared top), so its panic banner's call chain INCLUDES xcontam_*.
#     Post-fix the victim's own per-process trace holds only xvictim_* + victim.
#
# OBSERVABLE: the victim is the ONLY process that panics, so its banner is the only
# call chain printed to stderr. The harness (scripts/gc_trace_check.sh) asserts:
#     PASS  stderr's panic chain contains "xvictim" and does NOT contain "xcontam"
#     FAIL  stderr contains "xcontam" (a foreign process's frames leaked in)
# The victim is a NON-root process, so its panic prints the trace + kills only
# itself; main then exits 0 normally.

module Main

# Deep, distinctively-named contaminant chain. Parks ~forever at the bottom.
fun xcontam_e() { sleep(100000) }
fun xcontam_d() { xcontam_e() }
fun xcontam_c() { xcontam_d() }
fun xcontam_b() { xcontam_c() }
fun xcontam_a() { xcontam_b() }
fun contaminant() { xcontam_a() }

# Short, distinctively-named victim chain. Parks briefly, then panics. The
# calls aren't tail calls, so its callers stay in the chain: a stackless
# victim resumes from the heap with only xvictim_b on the carrier, and the
# panic must still print xvictim_a and victim (Phase 2g walks its frames).
# It panics in the statement it parked in, so the banner's line is the one
# it saved when it parked, not whatever its new carrier held.
fun xvictim_b() {
    t = "XVICTIM_"
    panic(receive { 'go' -> t ++ "PANIC" })
}
fun xvictim_a() { r = xvictim_b() ; r }
fun victim() { r = xvictim_a() ; r }

fun main() {
    print("trace_xproc_repro start (run with SW_SCHEDULERS=1)")
    spawn(contaminant())   # runs while main sleeps below; parks deep (top high pre-fix)
    sleep(20)              # let the contaminant reach its deep sleep() and park
    v = spawn(victim())    # builds its short chain and parks in a receive
    sleep(20)
    send(v, 'go')          # resumes it; it panics
    sleep(300)             # outlive the panic + stderr flush
    print("trace_xproc_repro done")
    sys_exit(0)
}
