module Test_observability

# Phase-4 observability: the extended process_info counters (memory /
# mailbox_len / messages_sent / messages_recv) and the swarm_stats()
# node-level metrics map (spawns / sends / crashes / restarts / drop
# counters / per-scheduler stats). Compiled suite: needs the real
# scheduler. Interpreter twin: tests/sw/run/test_observability_interp.sw.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun echo() {
    receive {
        {'ping', from} -> send(from, 'pong') ; echo()
        'die' -> 'ok'
    }
}

# Parks on a pattern nothing sends until released — messages sent to it
# stay UNDRAINED in the mailbox, so mailbox_len is deterministic.
fun parked() {
    receive { 'release' -> 'ok' }
}

fun crasher() { panic("obs_crash_counter") }

# --- process_info: the new per-process counters --------------------------
fun test_process_info_counters() {
    p = spawn(echo())
    send(p, {'ping', self()})
    receive { 'pong' -> 'ok' }
    me = process_info(self())
    fails = 0
    fails = fails + assert_eq("pi_memory_present", map_get(me, 'memory') == nil, false)
    fails = fails + assert_eq("pi_memory_positive", map_get(me, 'memory') > 0, true)
    fails = fails + assert_eq("pi_sent_counts", map_get(me, 'messages_sent') >= 1, true)
    fails = fails + assert_eq("pi_recv_counts", map_get(me, 'messages_recv') >= 1, true)
    worker = process_info(p)
    fails = fails + assert_eq("pi_worker_sent", map_get(worker, 'messages_sent') >= 1, true)
    fails = fails + assert_eq("pi_worker_recv", map_get(worker, 'messages_recv') >= 1, true)
    # the pre-existing keys must survive the extension
    fails = fails + assert_eq("pi_reductions_kept", map_get(me, 'reductions') >= 0, true)
    fails = fails + assert_eq("pi_status_kept", map_get(me, 'status'), 'running')
    send(p, 'die')
    fails
}

# --- mailbox_len: pending (undrained) depth ------------------------------
fun test_mailbox_depth() {
    p = spawn(parked())
    send(p, 'a')
    send(p, 'b')
    send(p, 'c')
    # sends are synchronous admission — mb_len is 3 the moment send returns,
    # and parked()'s selective receive skips (never consumes) them.
    info = process_info(p)
    fails = assert_eq("mb_len_pending", map_get(info, 'mailbox_len'), 3)
    fails = fails + assert_eq("mb_len_self_drained",
                              map_get(process_info(self()), 'mailbox_len'), 0)
    send(p, 'release')
    fails
}

# --- swarm_stats: map shape + monotonic counters --------------------------
fun test_swarm_stats() {
    s0 = swarm_stats()
    fails = 0
    fails = fails + assert_eq("ss_schedulers", map_get(s0, 'schedulers') >= 1, true)
    fails = fails + assert_eq("ss_processes", map_get(s0, 'processes') >= 1, true)
    fails = fails + assert_eq("ss_mailbox_dropped", map_get(s0, 'mailbox_dropped') >= 0, true)
    fails = fails + assert_eq("ss_msgsize_dropped", map_get(s0, 'msgsize_dropped') >= 0, true)
    fails = fails + assert_eq("ss_overflow_queue", map_get(s0, 'overflow_queue') >= 0, true)

    p = spawn(echo())
    send(p, {'ping', self()})
    receive { 'pong' -> 'ok' }
    s1 = swarm_stats()
    fails = fails + assert_eq("ss_spawns_inc",
                              map_get(s1, 'spawns') >= (map_get(s0, 'spawns') + 1), true)

    scheds = map_get(s1, 'scheduler_stats')
    fails = fails + assert_eq("ss_sched_list_len", length(scheds), map_get(s1, 'schedulers'))
    sc = hd(scheds)
    fails = fails + assert_eq("ss_sched_id", map_get(sc, 'id') >= 0, true)
    fails = fails + assert_eq("ss_sched_procs_run", map_get(sc, 'procs_run') >= 0, true)
    fails = fails + assert_eq("ss_sched_loop_iters", map_get(sc, 'loop_iters') >= 0, true)
    send(p, 'die')
    fails
}

# --- crashes: abnormal exits bump the node counter ------------------------
fun test_crash_counter() {
    c0 = map_get(swarm_stats(), 'crashes')
    res = spawn_monitor(crasher())
    ref = elem(res, 1)
    fails = receive {
        {'DOWN', dref, _, _, _} -> assert_eq("crash_down_ref", dref == ref, true)
        after 2000 { assert_eq("crash_down", "TIMEOUT", "DOWN") }
    }
    c1 = map_get(swarm_stats(), 'crashes')
    fails = fails + assert_eq("ss_crashes_inc", c1 >= (c0 + 1), true)
    fails
}

# --- restarts: a permanent crashing child bumps the restart counter -------
fun test_restart_counter() {
    r0 = map_get(swarm_stats(), 'restarts')
    sup = dyn_supervisor()
    sup_start_child(sup, {'obs_restarter', fun() { panic("obs_restart") }, 'permanent'})
    sleep(400)
    r1 = map_get(swarm_stats(), 'restarts')
    assert_eq("ss_restarts_inc", r1 >= (r0 + 1), true)
}

fun main() {
    fails = 0
    fails = fails + test_process_info_counters()
    fails = fails + test_mailbox_depth()
    fails = fails + test_swarm_stats()
    fails = fails + test_crash_counter()
    fails = fails + test_restart_counter()
    if (fails == 0) { print("OK observability 23/23") ; sys_exit(0) }
    else { print("FAIL observability " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
