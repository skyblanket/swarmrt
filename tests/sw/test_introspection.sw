module Test_introspection

# Swarm.top()/tree() live introspection (lib/Swarm.sw over process_list/
# process_info + the new parent field). Compiled-only: it reads the live
# scheduler's process table, which the interpreter doesn't have — so this runs
# only on the compile-then-execute path, like test_dynsup.sw.

import Swarm

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun pinger() {
    receive {
        {'ping', from} -> send(from, 'pong') ; pinger()
        'die' -> 'ok'
    }
}

# first row whose 'name' == target (a string), or nil.
fun find_named(rows, target) {
    if (length(rows) == 0) { nil }
    else {
        if (map_get(hd(rows), 'name') == target) { hd(rows) }
        else { find_named(tl(rows), target) }
    }
}

fun status_ok(s) {
    if (s == 'running') { 'true' }
    else { if (s == 'waiting') { 'true' }
    else { if (s == 'runnable') { 'true' } else { 'false' } } }
}

# top(): the snapshot includes a process we registered, as a map with pid+status.
fun test_top_snapshot() {
    sup = dyn_supervisor()
    sup_start_child(sup, {'tw_worker', fun() { pinger() }, 'permanent'})
    sleep(80)
    rows = Swarm.top()
    fails = 0
    fails = fails + assert_eq("top_nonempty", length(rows) == 0, false)
    row = find_named(rows, "tw_worker")
    fails = fails + assert_eq("worker_found", row == nil, false)
    fails = fails + assert_eq("row_has_pid", map_get(row, 'pid') == nil, false)
    fails = fails + assert_eq("row_status_ok", status_ok(map_get(row, 'status')), 'true')
    fails
}

# the parent link: the worker's 'parent' == the supervisor's numeric pid.
fun test_parent_link() {
    sup = dyn_supervisor()
    sup_start_child(sup, {'pl_worker', fun() { pinger() }, 'permanent'})
    sleep(80)
    sup_pid = map_get(process_info(sup), 'pid')
    row = find_named(Swarm.top(), "pl_worker")
    fails = assert_eq("pl_worker_found", row == nil, false)
    fails = fails + assert_eq("parent_is_supervisor", map_get(row, 'parent'), sup_pid)
    fails
}

# tree(): walking the forest reaches the worker nested under its supervisor.
fun flatten(forest, acc) {
    if (length(forest) == 0) { acc }
    else {
        node = hd(forest)
        acc2 = flatten(map_get(node, 'children'), [node | acc])
        flatten(tl(forest), acc2)
    }
}

fun find_pid(nodes, target) {
    if (length(nodes) == 0) { nil }
    else {
        if (map_get(hd(nodes), 'pid') == target) { hd(nodes) }
        else { find_pid(tl(nodes), target) }
    }
}

fun test_tree_groups_children() {
    sup = dyn_supervisor()
    sup_start_child(sup, {'tr_worker', fun() { pinger() }, 'permanent'})
    sleep(80)
    sup_pid = map_get(process_info(sup), 'pid')
    forest = Swarm.tree()
    fails = assert_eq("tree_nonempty", length(forest) == 0, false)
    all_nodes = flatten(forest, [])
    sup_node = find_pid(all_nodes, sup_pid)
    fails = fails + assert_eq("sup_node_found", sup_node == nil, false)
    child = find_named(map_get(sup_node, 'children'), "tr_worker")
    fails = fails + assert_eq("worker_under_supervisor", child == nil, false)
    fails
}

fun main() {
    fails = 0
    fails = fails + test_top_snapshot()
    fails = fails + test_parent_link()
    fails = fails + test_tree_groups_children()
    if (fails == 0) { print("OK introspection 9/9") ; sys_exit(0) }
    else { print("FAIL introspection " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
