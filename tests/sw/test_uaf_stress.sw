# test_uaf_stress.sw — copy-on-escape / use-after-free MESSAGING STRESS harness.
#
# Goal: surface a missed deep_copy_global at any value-escape boundary in the
# COMPILED runtime. Every worker BUILDS a compound value in ITS OWN process
# arena, hands it to a longer-lived reader (the parent / a registry / ETS),
# then EXITS. Once that worker's process_destroy() frees its varena, any
# sw_val_t* the reader still holds that was NOT deep-copied to the global heap
# is a dangling pointer.
#
# Build libswarmrt with  -DSW_ARENA_POISON  (free-fill arena chunks with 0xDE)
# and  -fsanitize=address . Then a missed escape manifests as either:
#   * ASAN: heap-use-after-free when the parent reads the value, OR
#   * POISON: the read returns 0xDE garbage -> a CONTENT assert below fails
#             loudly ("FAIL ..."), OR a structural deref faults.
#
# The harness asserts the EXACT content the workers sent. If copy-on-escape is
# correct, every assert passes and it prints "OK uaf_stress N/N" + exits 0.
# A miss => non-zero exit (assert mismatch) or a fault (ASAN/SEGV).
#
# Covered escape surfaces:
#   1. plain send/receive of tuples, lists, maps, strings (worker exits first)
#   2. spawn with CAPTURED compound args (closure capture must deep-copy)
#   3. pmap (each fn in its own process; results cross back to parent)
#   4. Std.task_stream (bounded fan-out; tagged results cross back)
#   5. ETS put by a dying worker, get by the parent (store must survive)
#   6. nested/deep compound (map of lists of tuples of strings) over send
#   7. fan-in: many short-lived workers, parent drains after they're gone

module Test_uaf_stress

import Std

# ---- assert helpers ----
fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# 0-indexed LIST accessor (elem is the TUPLE accessor; it panics on a list).
fun lat(lst, i) {
    if (i == 0) { hd(lst) } else { lat(tl(lst), i - 1) }
}

# Force a worker to be GONE before the parent reads what it sent: give the
# scheduler time to run process_exit -> process_destroy -> varena_free_all.
fun settle() { sleep(120) }

# ============================================================
# 1. plain send/receive of compound values; worker exits first
# ============================================================
# The worker builds a tuple {list, map, string} in its arena, sends it, then
# RETURNS (its fun ends -> process exits -> arena freed). The parent only
# reads AFTER settle(), so the bytes it reads must be a global-heap copy.
fun compound_sender(parent) {
    payload = {'data',
               [1, 2, 3, "four", 'five'],
               %{name: "worker", tags: ['a', 'b', 'c'], n: 42},
               "a string that lived in the worker arena"}
    send(parent, payload)
    'done'   # function returns -> worker process exits, arena is freed
}

fun test_plain_compound() {
    spawn(compound_sender(self()))
    msg = receive { m -> m }     # grab it while sender may still be alive
    settle()                     # NOW the sender is definitely gone + freed
    # Touch every nested field AFTER the sender's arena is freed.
    f0 = assert_eq("plain_tag",   elem(msg, 0), 'data')
    lst = elem(msg, 1)
    f1 = assert_eq("plain_list",  lst, [1, 2, 3, "four", 'five'])
    f2 = assert_eq("plain_list_str", lat(lst, 3), "four")
    m  = elem(msg, 2)
    f3 = assert_eq("plain_map_name", map_get(m, 'name'), "worker")
    f4 = assert_eq("plain_map_tags", map_get(m, 'tags'), ['a', 'b', 'c'])
    f5 = assert_eq("plain_map_n",    map_get(m, 'n'), 42)
    f6 = assert_eq("plain_string", elem(msg, 3),
                   "a string that lived in the worker arena")
    f0 + f1 + f2 + f3 + f4 + f5 + f6
}

# ============================================================
# 2. spawn with CAPTURED compound args
# ============================================================
# The parent builds a compound value, then spawns a closure that CAPTURES it.
# The captured value must be deep-copied into the child so that when the parent
# (the builder) keeps running / mutating its arena, the child's copy is intact.
# The child writes what it saw into ETS; the parent reads it back.
fun test_spawn_capture_compound() {
    t = ets_new()
    captured = %{rows: [{1, "one"}, {2, "two"}, {3, "three"}],
                 meta: %{kind: 'capture', ok: 'true'}}
    spawn(fn() {
        # Read the captured compound and stash exactly what we saw.
        rows = map_get(captured, 'rows')
        ets_put(t, 'seen_rows', rows)
        ets_put(t, 'seen_kind', map_get(map_get(captured, 'meta'), 'kind'))
    })
    settle()
    f0 = assert_eq("capture_rows", ets_get(t, 'seen_rows'),
                   [{1, "one"}, {2, "two"}, {3, "three"}])
    f1 = assert_eq("capture_kind", ets_get(t, 'seen_kind'), 'capture')
    f0 + f1
}

# ============================================================
# 3. pmap — each fn runs in its OWN process; result crosses back
# ============================================================
# Each pmap worker builds a fresh compound value, then its process exits and
# its arena is freed. The result list the parent holds must be global-heap
# copies, not pointers into the (now-freed) worker arenas.
fun test_pmap_compound() {
    res = pmap(fun(x) {
        {'item', x, [x, x * 10, x * 100], %{sq: x * x, label: "n" ++ to_string(x)}}
    }, [1, 2, 3, 4, 5])
    settle()   # all pmap workers are done + freed
    # Read every element's nested structure after the workers are gone.
    e2 = lat(res, 2)
    f0 = assert_eq("pmap_len", length(res), 5)
    f1 = assert_eq("pmap_tag", elem(e2, 0), 'item')
    f2 = assert_eq("pmap_idx", elem(e2, 1), 3)
    f3 = assert_eq("pmap_list", elem(e2, 2), [3, 30, 300])
    f4 = assert_eq("pmap_map_sq", map_get(elem(e2, 3), 'sq'), 9)
    f5 = assert_eq("pmap_map_label", map_get(elem(e2, 3), 'label'), "n3")
    f0 + f1 + f2 + f3 + f4 + f5
}

# ============================================================
# 4. Std.task_stream — bounded fan-out; tagged results cross back
# ============================================================
fun test_task_stream_compound() {
    res = Std.task_stream([1, 2, 3, 4, 5, 6], fun(x) {
        %{n: x, doubled: x * 2, words: "v-" ++ to_string(x)}
    }, %{max_concurrency: 2})
    settle()
    c0 = lat(res, 0)            # {'ok', %{...}}
    c5 = lat(res, 5)
    f0 = assert_eq("ts_len", length(res), 6)
    f1 = assert_eq("ts_first_tag", elem(c0, 0), 'ok')
    f2 = assert_eq("ts_first_n", map_get(elem(c0, 1), 'n'), 1)
    f3 = assert_eq("ts_last_doubled", map_get(elem(c5, 1), 'doubled'), 12)
    f4 = assert_eq("ts_last_words", map_get(elem(c5, 1), 'words'), "v-6")
    f0 + f1 + f2 + f3 + f4
}

# ============================================================
# 5. ETS put by a DYING worker, get by the parent
# ============================================================
# A worker writes a compound value into a shared ETS table, then exits. The
# table outlives the worker, so the stored key+value must be deep-copied global
# (this is the ets_put store path the audit flagged as fix-separately — this
# test will catch it if the deep-copy is missing once varena is live).
fun ets_writer(t, key) {
    ets_put(t, key, %{owner: key,
                      payload: ["x", "y", "z"],
                      nested: {'tuple', %{deep: [1, [2, [3, []]]]}}})
    'done'   # worker exits, arena freed
}

fun test_ets_cross_process() {
    t = ets_new()
    spawn(ets_writer(t, 'w1'))
    spawn(ets_writer(t, 'w2'))
    spawn(ets_writer(t, 'w3'))
    settle()   # all three writers gone + freed
    v1 = ets_get(t, 'w1')
    v3 = ets_get(t, 'w3')
    f0 = assert_eq("ets_count", ets_count(t), 3)
    f1 = assert_eq("ets_w1_owner", map_get(v1, 'owner'), 'w1')
    f2 = assert_eq("ets_w1_payload", map_get(v1, 'payload'), ["x", "y", "z"])
    # Deep-structure check on w3's nested tuple's map's list.
    deep = map_get(elem(map_get(v3, 'nested'), 1), 'deep')
    f3 = assert_eq("ets_w3_deep", deep, [1, [2, [3, []]]])
    f0 + f1 + f2 + f3
}

# ============================================================
# 6. deeply nested compound over send (map of lists of tuples of strings)
# ============================================================
fun deep_sender(parent) {
    v = %{
        level0: [
            {'pair', "k1", ["a1", "a2"]},
            {'pair', "k2", ["b1", "b2", "b3"]}
        ],
        level1: %{
            inner: [%{deep: {'leaf', "buried string", [9, 8, 7]}}]
        }
    }
    send(parent, {'deep', v})
    'done'
}

fun test_deep_nested() {
    spawn(deep_sender(self()))
    msg = receive { m -> m }
    settle()
    v = elem(msg, 1)
    l0 = map_get(v, 'level0')
    pair2 = lat(l0, 1)
    f0 = assert_eq("deep_pair2_key", elem(pair2, 1), "k2")
    f1 = assert_eq("deep_pair2_vals", elem(pair2, 2), ["b1", "b2", "b3"])
    inner = lat(map_get(map_get(v, 'level1'), 'inner'), 0)
    leaf = map_get(inner, 'deep')
    f2 = assert_eq("deep_leaf_tag", elem(leaf, 0), 'leaf')
    f3 = assert_eq("deep_leaf_str", elem(leaf, 1), "buried string")
    f4 = assert_eq("deep_leaf_nums", elem(leaf, 2), [9, 8, 7])
    f0 + f1 + f2 + f3 + f4
}

# ============================================================
# 7. fan-in: many short-lived workers, parent drains AFTER they're gone
# ============================================================
# Spawn N workers that each send a distinct tagged compound and immediately
# exit. The parent sleeps so ALL workers are gone+freed, THEN drains the
# mailbox and verifies each message survived its sender's arena teardown.
fun fanin_worker(parent, id) {
    send(parent, {'fanin', id, ["payload", id, id * id], %{id: id}})
    'done'
}

fun spawn_fanin(parent, n) {
    if (n <= 0) { 'ok' }
    else { spawn(fanin_worker(parent, n)) ; spawn_fanin(parent, n - 1) }
}

# Collect `count` messages into a list with a BLOCKING receive (reliable
# delivery — the parent yields to the schedulers on each receive, so every
# worker runs, sends, and exits). We deliberately do NOT touch the message
# internals here; we only stash whole messages. The deref-after-free happens
# later, in verify_fanin, once every sender has been freed.
fun collect_fanin(count, acc) {
    if (count <= 0) { acc }
    else {
        m = receive { x -> x ; after 5000 { 'timeout' } }
        if (m == 'timeout') {
            print("FAIL fanin: timed out with " ++ to_string(count) ++ " left")
            acc
        } else {
            collect_fanin(count - 1, list_append(acc, m))
        }
    }
}

# Walk the collected list, summing ids and asserting each message is
# structurally intact. By the time this runs, every sender's arena has been
# freed (settle() in test_fanin), so each touch is a read-after-free probe.
fun verify_fanin(msgs, sum_acc) {
    if (length(msgs) == 0) { sum_acc }
    else {
        m = hd(msgs)
        id = elem(m, 1)
        lst = elem(m, 2)
        mp = elem(m, 3)
        ok = (elem(m, 0) == 'fanin') &&
             (lat(lst, 0) == "payload") &&
             (lat(lst, 2) == id * id) &&
             (map_get(mp, 'id') == id)
        if (ok == 'true') { verify_fanin(tl(msgs), sum_acc + id) }
        else {
            print("FAIL fanin_struct id=" ++ to_string(id))
            verify_fanin(tl(msgs), sum_acc)
        }
    }
}

fun test_fanin() {
    n = 25
    spawn_fanin(self(), n)
    msgs = collect_fanin(n, [])    # reliable blocking collection
    settle()                       # NOW every sender is gone + freed
    settle()
    total = verify_fanin(msgs, 0)  # read every nested field post-free
    # sum of ids 1..25 = 325. A wrong total means a message was corrupted/lost.
    assert_eq("fanin_sum", total, 325)
}

fun main() {
    fails = 0
    fails = fails + test_plain_compound()
    fails = fails + test_spawn_capture_compound()
    fails = fails + test_pmap_compound()
    fails = fails + test_task_stream_compound()
    fails = fails + test_ets_cross_process()
    fails = fails + test_deep_nested()
    fails = fails + test_fanin()
    if (fails == 0) { print("OK uaf_stress 7/7 groups") ; sys_exit(0) }
    else { print("FAIL uaf_stress " ++ to_string(fails) ++ " failing asserts") ; sys_exit(1) }
}
