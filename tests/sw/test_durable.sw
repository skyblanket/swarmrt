module Test_durable

# Durable agent state: checkpoint(key, value) / restore(key) /
# checkpoint_delete(key) persist sw values in SQLite ($SW_STATE_DB) so they
# survive the OS process dying.
#
# The driver (no SW_DURABLE_MODE) picks a fresh temp db and re-runs this
# binary with SW_STATE_DB pointing at it, once per scenario:
#   roundtrip  — every value type comes back identical (tuple stays tuple,
#                atom stays atom, bytes stay binary-clean); rejections.
#   crash      — counts upward checkpointing each step, then SIGKILLs itself
#                mid-step; the next run resumes from the last checkpoint.
#   loop       — Durable.loop killed mid-run, re-run finishes the job.
#   supervised — a dyn_supervisor child that panics restores its state
#                after the in-process restart.

import Durable
import Std

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# Checkpoint `v` under `key`, read it back, compare structurally + by type.
fun rt(name, v) {
    ok = checkpoint("rt:" ++ name, v)
    back = restore("rt:" ++ name)
    assert_eq("rt_" ++ name, {ok, back, typeof(back)}, {'ok', v, typeof(v)})
}

fun roundtrip() {
    fails = 0
    fails = fails + rt("int", 42)
    fails = fails + rt("neg_int", 0 - 9007199254740993)
    fails = fails + rt("float", 3.25)
    fails = fails + rt("neg_float", 0.0 - 0.000123)
    fails = fails + rt("string", "hello, durable")
    fails = fails + rt("empty_string", "")
    fails = fails + rt("utf8_string", "héllo — 世界")
    fails = fails + rt("atom", 'running')
    fails = fails + rt("list", [1, 2, 3])
    fails = fails + rt("empty_list", [])
    fails = fails + rt("tuple", {'ok', 1, "x"})
    fails = fails + rt("map", %{"n" => 1, 'k' => 'v', 7 => [1.5]})
    fails = fails + rt("empty_map", map_new())
    fails = fails + rt("bytes", bytes_from_ints([0, 1, 0, 255, 128]))
    fails = fails + rt("nested", %{"agent" => {'state', [%{"t" => {1, [2, {3}]}}, 'idle']},
                                   "log" => ["a", 'b', 2.5, bytes_from_ints([0])]})

    # The distinctions JSON would lose must survive.
    fails = fails + assert_eq("tuple_not_list", restore("rt:tuple") == ['ok', 1, "x"], false)
    fails = fails + assert_eq("atom_not_string", restore("rt:atom") == "running", false)

    # Overwrite replaces; missing is nil; delete removes.
    checkpoint("ow", 1)
    checkpoint("ow", {'v', 2})
    fails = fails + assert_eq("overwrite", restore("ow"), {'v', 2})
    fails = fails + assert_eq("missing_nil", restore("never-written"), nil)
    fails = fails + assert_eq("delete_ok", checkpoint_delete("ow"), 'ok')
    fails = fails + assert_eq("deleted_nil", restore("ow"), nil)
    fails = fails + assert_eq("delete_missing_ok", checkpoint_delete("ow"), 'ok')

    # Values with no meaning in a later OS process are rejected, not stored.
    fails = fails + assert_eq("reject_pid", checkpoint("bad", self()), {'error', 'not_serializable'})
    fails = fails + assert_eq("reject_nested_pid", checkpoint("bad", [1, {'from', self()}]),
                              {'error', 'not_serializable'})
    fails = fails + assert_eq("reject_fun", checkpoint("bad", fun(x) { x }), {'error', 'not_serializable'})
    fails = fails + assert_eq("rejected_not_stored", restore("bad"), nil)
    fails = fails + assert_eq("reject_bad_key", checkpoint('atomkey', 1), {'error', 'bad_key'})
    fails = fails + assert_eq("restore_bad_key_nil", restore(1), nil)
    print("RESULT " ++ to_string(fails))
    sys_exit(0)
}

fun env_int(name, dflt) {
    v = getenv(name)
    if (v == nil || v == "") { dflt } else { to_int(v) }
}

# Count from the restored value up to `target`, checkpointing each step.
# With SW_CRASH_AT set, SIGKILL ourselves after computing that step but before
# checkpointing it — the worst case: the in-memory progress of that step is
# lost, everything before it must not be.
fun crash_count(i, target, crash_at, done) {
    if (i > target) { done }
    else {
        if (i == crash_at) { shell("kill -9 $PPID") ; sleep(5000) }
        checkpoint("counter", i)
        crash_count(i + 1, target, crash_at, list_append(done, i))
    }
}

fun crash_child() {
    start = Durable.load("counter", 0)
    crash_at = env_int("SW_CRASH_AT", 0)
    print("START " ++ to_string(start))
    done = crash_count(start + 1, 20, crash_at, [])
    print("STEPS " ++ to_string(done))
    sys_exit(0)
}

# Durable.loop: sum 1..10 one step at a time; die at step 6 on the first run.
fun loop_step(s, crash_at) {
    n = map_get(s, "n")
    if (n == crash_at) { shell("kill -9 $PPID") ; sleep(5000) }
    if (n < 10) { {'next', %{"n" => n + 1, "sum" => map_get(s, "sum") + n + 1, "runs" => map_get(s, "runs")}} }
    else { {'done', {map_get(s, "sum"), map_get(s, "runs")}} }
}

fun loop_child() {
    crash_at = env_int("SW_CRASH_AT", 0 - 1)
    # Count how many OS processes worked on this job (restored "runs" + 1).
    init = Durable.load("job", %{"n" => 0, "sum" => 0, "runs" => 0})
    s0 = map_put(init, "runs", map_get(init, "runs") + 1)
    Durable.save("job", s0)
    r = Durable.loop("job", s0, fun(s) { loop_step(s, crash_at) })
    print("LOOP " ++ to_string(r) ++ " " ++ to_string(restore("job")))
    sys_exit(0)
}

# A supervised worker whose count lives in the state db: after a panic the
# supervisor restarts it and it resumes from the checkpoint, not from 0.
fun sup_worker() { sup_worker_loop(Durable.load("sup_ctr", 0)) }

fun sup_worker_loop(n) {
    receive {
        'inc' -> Durable.save("sup_ctr", n + 1) ; sup_worker_loop(n + 1)
        {'get', from} -> send(from, {'count', n}) ; sup_worker_loop(n)
        'boom' -> panic("boom")
    }
}

fun ask(name) {
    send(whereis(name), {'get', self()})
    receive {
        {'count', n} -> n
        after 2000 { 'timeout' }
    }
}

fun supervised_child() {
    sup = dyn_supervisor()
    c = sup_start_child(sup, {'dur_worker', fun() { sup_worker() }, 'permanent'})
    send(c, 'inc') ; send(c, 'inc') ; send(c, 'inc')
    before = ask('dur_worker')
    send(c, 'boom')
    sleep(300)
    revived = whereis('dur_worker')
    after_restart = ask('dur_worker')
    send(revived, 'inc')
    final = ask('dur_worker')
    print("SUP " ++ to_string({before, revived != c, after_restart, final}))
    sys_exit(0)
}

fun run_child(me, db, mode, crash_at) {
    cmd = "SW_STATE_DB='" ++ db ++ "' SW_DURABLE_MODE=" ++ mode ++
          " SW_CRASH_AT=" ++ crash_at ++ " '" ++ me ++ "' 2>&1"
    r = shell(cmd)
    {elem(r, 0), string_trim(elem(r, 1))}
}

fun driver() {
    me = hd(os_args())
    db = file_temp("/tmp/sw_durable_test_")
    fails = 0

    r = run_child(me, db, "roundtrip", "")
    print(elem(r, 1))
    fails = fails + assert_eq("roundtrip_child", string_ends_with(elem(r, 1), "RESULT 0"), true)

    # Crash run: steps 1..7 checkpointed, killed while doing step 8.
    c1 = run_child(me, db, "crash", "8")
    fails = fails + assert_eq("crash_child_killed", elem(c1, 0) != 0, true)
    fails = fails + assert_eq("checkpoint_survives_kill", restore_via(me, db, "counter"), "VAL 7")
    # Resume run: picks up at 8, nothing lost, nothing repeated.
    c2 = run_child(me, db, "crash", "")
    fails = fails + assert_eq("resume_exit", elem(c2, 0), 0)
    fails = fails + assert_eq("resume_output", elem(c2, 1),
        "START 7\nSTEPS [8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20]")

    # Durable.loop: killed at n=6, re-run finishes; 2 OS processes did the job,
    # the sum is right (no step lost or double-counted), the key is cleared.
    l1 = run_child(me, db, "loop", "6")
    fails = fails + assert_eq("loop_child_killed", elem(l1, 0) != 0, true)
    l2 = run_child(me, db, "loop", "")
    fails = fails + assert_eq("loop_resumed_and_cleared", elem(l2, 1), "LOOP {55, 2} nil")

    s = run_child(me, db, "supervised", "")
    # {count before crash, restarted as a new pid, count after restart, +1}.
    # (The panic banner goes to stderr, merged ahead of the result line.)
    fails = fails + assert_eq("supervised_restore", last_line(elem(s, 1)), "SUP {3, :true, 3, 4}")

    shell("rm -f '" ++ db ++ "' '" ++ db ++ "-wal' '" ++ db ++ "-shm'")
    total = 36   # 28 in the roundtrip child + 8 here
    if (fails == 0) { print("OK durable " ++ to_string(total) ++ "/" ++ to_string(total)) ; sys_exit(0) }
    else { print("FAIL durable " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}

fun last_line(out) { Std.last(string_split(out, "\n")) }

# Read one key in a separate OS process (proves it is on disk, not cached).
fun restore_via(me, db, key) {
    r = shell("SW_STATE_DB='" ++ db ++ "' SW_DURABLE_MODE=peek SW_KEY=" ++ key ++ " '" ++ me ++ "' 2>&1")
    string_trim(elem(r, 1))
}

fun main() {
    case getenv("SW_DURABLE_MODE") {
        "roundtrip"  -> roundtrip()
        "crash"      -> crash_child()
        "loop"       -> loop_child()
        "supervised" -> supervised_child()
        "peek"       -> print("VAL " ++ to_string(restore(getenv("SW_KEY"))))
        _            -> driver()
    }
}
