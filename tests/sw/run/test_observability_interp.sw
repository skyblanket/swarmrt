# test_observability_interp.sw — interpreter-side coverage of the Phase-4
# observability surface: swarm_stats() and the extended process_info keys
# (memory / mailbox_len / messages_sent / messages_recv). Run via:
#
#   ./bin/swc run tests/sw/run/test_observability_interp.sw
#
# `swc run` boots a real single-scheduler runtime, so these are the SAME
# counters the compiled path reads (tests/sw/test_observability.sw is the
# compiled twin). Guards against codegen/interpreter drift on the new
# builtin. Note: interpreter values may live on the global heap (no per-
# process arena), so 'memory' is only asserted PRESENT here, not positive.

module Test_observability_interp

fun chk(name, actual, expected) {
    if (actual == expected) { 0 }
    else {
        print("OBS_INTERP_FAIL " ++ name ++ ": expected " ++
              to_string(expected) ++ ", got " ++ to_string(actual))
        1
    }
}

fun echo() {
    receive {
        {'ping', from} -> send(from, 'pong') ; echo()
        'die' -> 'ok'
    }
}

fun main() {
    fails = 0

    p = spawn(echo())
    send(p, {'ping', self()})
    receive { 'pong' -> 'ok' }

    info = process_info(self())
    fails = fails + chk("pi_memory_present", map_get(info, 'memory') == nil, false)
    fails = fails + chk("pi_mailbox_len_present", map_get(info, 'mailbox_len') == nil, false)
    fails = fails + chk("pi_sent", map_get(info, 'messages_sent') >= 1, true)
    fails = fails + chk("pi_recv", map_get(info, 'messages_recv') >= 1, true)

    s = swarm_stats()
    fails = fails + chk("ss_is_present", s == nil, false)
    fails = fails + chk("ss_processes", map_get(s, 'processes') >= 1, true)
    fails = fails + chk("ss_schedulers", map_get(s, 'schedulers') >= 1, true)
    fails = fails + chk("ss_spawns", map_get(s, 'spawns') >= 1, true)
    fails = fails + chk("ss_sched_list", length(map_get(s, 'scheduler_stats')),
                        map_get(s, 'schedulers'))

    send(p, 'die')
    if (fails == 0) { print("OBS_INTERP_OK") ; sys_exit(0) }
    else { sys_exit(1) }
}
