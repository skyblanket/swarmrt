# Swarm.sw — live introspection: see the processes you spawned and how they're
# supervised. Pure-sw over the process_list / process_info / registered builtins.
#
# COMPILED-ONLY: these read the live scheduler's process table, which the
# tree-walking interpreter doesn't have. Under `swc run` / the REPL the
# underlying primitives warn and return nil, so Swarm.top()/tree() are only
# meaningful in a `swc build` binary.

module Swarm

export [top, tree, print_top, print_tree]

# top() — flat snapshot: a list of per-process maps. Each map has:
#   pid (int), status, reductions, messages (drained mailbox depth),
#   heap_used, heap_size, and — when set — name and parent (parent's pid).
fun top() {
    map(fun(p) { process_info(p) }, process_list())
}

# tree() — the supervision forest. Returns the root processes (those with no
# live parent), each as its process map plus a 'children' key holding the
# recursively-built child nodes. Grouping is by the parent pid that
# process_info now reports (child->parent is set by spawn_link, so supervised
# children point at their supervisor).
fun tree() {
    rows = top()
    live = _pids(rows, [])
    roots = filter(fun(r) {
        par = map_get(r, 'parent')
        (par == nil) || (_member(par, live) == 'false')
    }, rows)
    map(fun(r) { _node(r, rows) }, roots)
}

fun _node(r, rows) {
    pid = map_get(r, 'pid')
    kids = filter(fun(c) { map_get(c, 'parent') == pid }, rows)
    map_put(r, 'children', map(fun(c) { _node(c, rows) }, kids))
}

# print_top() — a `top`-style table to stdout; returns the process count.
fun print_top() {
    rows = top()
    _each(rows, fun(r) {
        print(to_string(map_get(r, 'pid')) ++ "  " ++
              to_string(map_get(r, 'status')) ++ "  msgs=" ++
              to_string(map_get(r, 'messages')) ++ "  " ++ _label(r))
    })
    length(rows)
}

# print_tree() — an indented supervision tree to stdout; returns root count.
fun print_tree() {
    roots = tree()
    _each(roots, fun(r) { _print_node(r, 0) })
    length(roots)
}

fun _print_node(node, depth) {
    print(_spaces(depth * 2, "") ++ _label(node) ++
          " [" ++ to_string(map_get(node, 'status')) ++ "]")
    _each(map_get(node, 'children'), fun(c) { _print_node(c, depth + 1) })
}

# ---- internals ----

fun _label(r) {
    name = map_get(r, 'name')
    if (name == nil) { to_string(map_get(r, 'pid')) } else { name }
}

fun _pids(rows, acc) {
    if (length(rows) == 0) { reverse(acc) }
    else { _pids(tl(rows), [map_get(hd(rows), 'pid') | acc]) }
}

fun _member(x, lst) {
    if (length(lst) == 0) { 'false' }
    else { if (hd(lst) == x) { 'true' } else { _member(x, tl(lst)) } }
}

fun _each(lst, fn) {
    if (length(lst) == 0) { 'ok' }
    else { fn(hd(lst)) ; _each(tl(lst), fn) }
}

fun _spaces(n, acc) { if (n <= 0) { acc } else { _spaces(n - 1, acc ++ " ") } }

fun reverse(lst) { _rev(lst, []) }
fun _rev(lst, acc) {
    if (length(lst) == 0) { acc }
    else { _rev(tl(lst), [hd(lst) | acc]) }
}
