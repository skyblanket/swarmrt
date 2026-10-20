# Std.sw — sw standard library, written in sw.
#
# Auto-discoverable: `import Std` works from anywhere because swc
# falls back to <swarmrt-root>/lib/ after the local directory.
#
# Everything here is pure sw — no new C builtins required. The
# implementations are the canonical reference: when in doubt about
# what `partition` or `group_by` does, read the source below.

module Std

export [
    # ---- list ops ----
    each, repeat, range, take, drop, take_while, drop_while,
    zip, unzip, partition, group_by, sort, sort_by, reverse,
    flatten, unique, contains, find, any, all, count, last, init,
    chunk_every, intersperse, max_by, min_by, sum, product,
    reduce, reduce_while,
    # map / filter / reduce are GLOBAL builtins; Std.map / Std.filter /
    # Std.reduce / Std.join also work because that's what an Elixir-shaped
    # guess reaches for, and the miss used to surface as a raw linker error.
    # sum/product don't call the global `reduce` bare (they use _reduce_acc),
    # so Std.reduce can shadow it inside this module. join == string_join.
    map, filter, join,
    # ---- map ops ----
    map_each, map_filter,
    # ---- string ops ----
    string_join, string_pad_left, string_pad_right, string_repeat,
    string_indent,
    # ---- concurrency ----
    task_stream, task_stream_each,
    # ---- structured output ----
    json_expect
]

# ============================================================
# LIST OPS
# ============================================================

# each(lst, fn) — call fn(item) for side effects. Returns 'ok'.
fun each(lst, fn) {
    if (length(lst) == 0) { 'ok' }
    else { fn(hd(lst)) ; each(tl(lst), fn) }
}

# repeat(value, n) — list of n copies of value.
fun repeat(v, n) { _repeat_acc(v, n, []) }
fun _repeat_acc(v, n, acc) {
    if (n <= 0) { acc }
    else { _repeat_acc(v, n - 1, list_append(acc, v)) }
}

# range(from, to_exclusive) — [from, from+1, ..., to-1].
fun range(from, to) { _range_acc(from, to, []) }
fun _range_acc(i, to, acc) {
    if (i >= to) { acc }
    else { _range_acc(i + 1, to, list_append(acc, i)) }
}

# take(lst, n) — first n elements (or all of lst if shorter).
fun take(lst, n) { _take_acc(lst, n, []) }
fun _take_acc(lst, n, acc) {
    if (n <= 0 || length(lst) == 0) { acc }
    else { _take_acc(tl(lst), n - 1, list_append(acc, hd(lst))) }
}

# drop(lst, n) — all but the first n elements.
fun drop(lst, n) {
    if (n <= 0 || length(lst) == 0) { lst }
    else { drop(tl(lst), n - 1) }
}

# take_while(lst, pred) — prefix where pred(x) is 'true'.
fun take_while(lst, pred) { _take_while_acc(lst, pred, []) }
fun _take_while_acc(lst, pred, acc) {
    if (length(lst) == 0) { acc }
    else {
        h = hd(lst)
        if (pred(h) == 'true') { _take_while_acc(tl(lst), pred, list_append(acc, h)) }
        else { acc }
    }
}

# drop_while(lst, pred) — suffix starting from the first non-matching element.
fun drop_while(lst, pred) {
    if (length(lst) == 0) { lst }
    else { if (pred(hd(lst)) == 'true') { drop_while(tl(lst), pred) } else { lst } }
}

# zip(a, b) — pair up elements until either list ends.
fun zip(a, b) { _zip_acc(a, b, []) }
fun _zip_acc(a, b, acc) {
    if (length(a) == 0 || length(b) == 0) { acc }
    else { _zip_acc(tl(a), tl(b), list_append(acc, {hd(a), hd(b)})) }
}

# unzip([{a,b},...]) -> {[a...], [b...]}
fun unzip(pairs) { _unzip_acc(pairs, [], []) }
fun _unzip_acc(pairs, la, lb) {
    if (length(pairs) == 0) { {la, lb} }
    else {
        p = hd(pairs)
        _unzip_acc(tl(pairs), list_append(la, elem(p, 0)), list_append(lb, elem(p, 1)))
    }
}

# partition(lst, pred) -> {matches, rest}
fun partition(lst, pred) { _partition_acc(lst, pred, [], []) }
fun _partition_acc(lst, pred, yes, no) {
    if (length(lst) == 0) { {yes, no} }
    else {
        h = hd(lst)
        if (pred(h) == 'true') { _partition_acc(tl(lst), pred, list_append(yes, h), no) }
        else { _partition_acc(tl(lst), pred, yes, list_append(no, h)) }
    }
}

# group_by(lst, key_fn) -> %{key: [items...], ...}
fun group_by(lst, key_fn) { _group_acc(lst, key_fn, %{}) }
fun _group_acc(lst, key_fn, acc) {
    if (length(lst) == 0) { acc }
    else {
        h = hd(lst)
        k = key_fn(h)
        existing = map_get(acc, k)
        bucket = case existing { nil -> [h] ; b -> list_append(b, h) }
        _group_acc(tl(lst), key_fn, map_put(acc, k, bucket))
    }
}

# sort(lst) — generic ascending sort using `<`. O(n^2) insertion sort
# (good enough for small N; rewrite as merge sort when you hit a real
# performance ceiling).
fun sort(lst) { sort_by(lst, fun(x) { x }) }

# sort_by(lst, key_fn) — sort ascending by key.
fun sort_by(lst, key_fn) { _sort_insert(lst, key_fn, []) }
fun _sort_insert(remaining, key_fn, sorted) {
    if (length(remaining) == 0) { sorted }
    else { _sort_insert(tl(remaining), key_fn, _insert_one(hd(remaining), key_fn, sorted, [])) }
}
fun _insert_one(item, key_fn, sorted, acc) {
    if (length(sorted) == 0) { list_append(acc, item) }
    else {
        h = hd(sorted)
        if (key_fn(item) < key_fn(h)) { acc ++ [item] ++ sorted }
        else { _insert_one(item, key_fn, tl(sorted), list_append(acc, h)) }
    }
}

# reverse(lst)
fun reverse(lst) { _reverse_acc(lst, []) }
fun _reverse_acc(lst, acc) {
    if (length(lst) == 0) { acc }
    else { _reverse_acc(tl(lst), [hd(lst)] ++ acc) }
}

# flatten([[a, b], [c]]) -> [a, b, c] — one level deep.
fun flatten(lists) { _flatten_acc(lists, []) }
fun _flatten_acc(lists, acc) {
    if (length(lists) == 0) { acc }
    else { _flatten_acc(tl(lists), acc ++ hd(lists)) }
}

# unique(lst) — preserves order of first occurrence.
fun unique(lst) { _unique_acc(lst, []) }
fun _unique_acc(lst, acc) {
    if (length(lst) == 0) { acc }
    else {
        h = hd(lst)
        if (contains(acc, h) == 'true') { _unique_acc(tl(lst), acc) }
        else { _unique_acc(tl(lst), list_append(acc, h)) }
    }
}

# contains(lst, value) -> 'true' / 'false'
fun contains(lst, v) {
    if (length(lst) == 0) { 'false' }
    else { if (hd(lst) == v) { 'true' } else { contains(tl(lst), v) } }
}

# find(lst, pred) -> first matching item, or nil.
fun find(lst, pred) {
    if (length(lst) == 0) { nil }
    else { if (pred(hd(lst)) == 'true') { hd(lst) } else { find(tl(lst), pred) } }
}

# any(lst, pred) -> 'true' if any element satisfies pred.
fun any(lst, pred) {
    if (length(lst) == 0) { 'false' }
    else { if (pred(hd(lst)) == 'true') { 'true' } else { any(tl(lst), pred) } }
}

# all(lst, pred) -> 'true' if every element satisfies pred.
fun all(lst, pred) {
    if (length(lst) == 0) { 'true' }
    else { if (pred(hd(lst)) == 'false') { 'false' } else { all(tl(lst), pred) } }
}

# count(lst, pred) -> int. Number of elements where pred is truthy.
fun count(lst, pred) { _count_acc(lst, pred, 0) }
fun _count_acc(lst, pred, n) {
    if (length(lst) == 0) { n }
    else {
        h = hd(lst)
        delta = if (pred(h) == 'true') { 1 } else { 0 }
        _count_acc(tl(lst), pred, n + delta)
    }
}

# last(lst) -> last element. Panics on empty.
fun last(lst) {
    if (length(lst) == 0) { panic("Std.last: empty list") }
    else { if (length(lst) == 1) { hd(lst) } else { last(tl(lst)) } }
}

# init(lst) -> everything but the last element. Panics on empty.
fun init(lst) {
    if (length(lst) == 0) { panic("Std.init: empty list") }
    else { take(lst, length(lst) - 1) }
}

# nth(lst, i) -> 0-indexed element. Panics if i is out of range.
# `elem/2` is the tuple-only positional accessor; this is the list
# equivalent.  `Std.at` is an alias for the same operation.
fun nth(lst, i) {
    if (i < 0) { panic("Std.nth: negative index") }
    else { if (length(lst) <= i) { panic("Std.nth: index out of range") }
           else { if (i == 0) { hd(lst) } else { nth(tl(lst), i - 1) } } }
}

fun at(lst, i) { nth(lst, i) }

# chunk_every(lst, size) -> list of size-N sublists. Last chunk may be shorter.
fun chunk_every(lst, size) { _chunk_acc(lst, size, []) }
fun _chunk_acc(lst, size, acc) {
    if (length(lst) == 0) { acc }
    else { _chunk_acc(drop(lst, size), size, list_append(acc, take(lst, size))) }
}

# intersperse(lst, sep) -> [a, sep, b, sep, c]
fun intersperse(lst, sep) {
    if (length(lst) <= 1) { lst }
    else { [hd(lst), sep] ++ intersperse(tl(lst), sep) }
}

# max_by(lst, key_fn) -> item with highest key. Panics on empty.
fun max_by(lst, key_fn) {
    if (length(lst) == 0) { panic("Std.max_by: empty list") }
    else { _max_by_acc(tl(lst), key_fn, hd(lst), key_fn(hd(lst))) }
}
fun _max_by_acc(lst, key_fn, best, best_key) {
    if (length(lst) == 0) { best }
    else {
        h = hd(lst)
        k = key_fn(h)
        if (k > best_key) { _max_by_acc(tl(lst), key_fn, h, k) }
        else { _max_by_acc(tl(lst), key_fn, best, best_key) }
    }
}

# min_by(lst, key_fn) -> item with lowest key. Panics on empty.
fun min_by(lst, key_fn) {
    if (length(lst) == 0) { panic("Std.min_by: empty list") }
    else { _min_by_acc(tl(lst), key_fn, hd(lst), key_fn(hd(lst))) }
}
fun _min_by_acc(lst, key_fn, best, best_key) {
    if (length(lst) == 0) { best }
    else {
        h = hd(lst)
        k = key_fn(h)
        if (k < best_key) { _min_by_acc(tl(lst), key_fn, h, k) }
        else { _min_by_acc(tl(lst), key_fn, best, best_key) }
    }
}

# reduce(lst, init, fn(acc, x)) — left fold. Returns init for an empty list.
# Tail-recursive, so a long list runs in flat stack.
fun reduce(lst, init, f) { _reduce_acc(lst, init, f) }
fun _reduce_acc(lst, acc, f) {
    if (length(lst) == 0) { acc }
    else { _reduce_acc(tl(lst), f(acc, hd(lst)), f) }
}

# reduce_while(lst, init, fn(acc, x)) — like reduce, but fn returns
# {'cont', acc} to keep going or {'halt', acc} to stop early.
fun reduce_while(lst, init, f) { _reduce_while_acc(lst, init, f) }
fun _reduce_while_acc(lst, acc, f) {
    if (length(lst) == 0) { acc }
    else {
        step = f(acc, hd(lst))
        if (elem(step, 0) == 'halt') { elem(step, 1) }
        else { _reduce_while_acc(tl(lst), elem(step, 1), f) }
    }
}

# sum(lst) — assumes numeric elements.
fun sum(lst) { _reduce_acc(lst, 0, fun(acc, x) { acc + x }) }

# product(lst) — assumes numeric elements.
fun product(lst) { _reduce_acc(lst, 1, fun(acc, x) { acc * x }) }

# ============================================================
# MAP OPS — sw maps are immutable; helpers return new maps.
# ============================================================

# map_each(m, fn) — call fn(k, v) for each pair (side effects).
fun map_each(m, fn) { _map_each(map_keys(m), m, fn) }
fun _map_each(ks, m, fn) {
    if (length(ks) == 0) { 'ok' }
    else { k = hd(ks) ; fn(k, map_get(m, k)) ; _map_each(tl(ks), m, fn) }
}

# map_filter(m, pred) — keep entries where pred(k, v) is truthy.
fun map_filter(m, pred) { _map_filter(map_keys(m), m, pred, %{}) }
fun _map_filter(ks, m, pred, acc) {
    if (length(ks) == 0) { acc }
    else {
        k = hd(ks)
        v = map_get(m, k)
        next_acc = if (pred(k, v) == 'true') { map_put(acc, k, v) } else { acc }
        _map_filter(tl(ks), m, pred, next_acc)
    }
}

# ============================================================
# STRING OPS
# ============================================================

# Std.map / Std.filter — the global builtins exist, but
# Std.map / Std.filter is what an Elixir-shaped guess reaches for, and
# the miss used to die as a raw linker error. Self-contained (not
# delegating to the same-named builtin) and order-tolerant like the
# globals: the list may be either of the first two arguments.
fun map(a, b) {
    if (typeof(a) == "list") { _map_acc(a, b, []) }
    else { _map_acc(b, a, []) }
}
fun _map_acc(lst, f, acc) {
    if (length(lst) == 0) { reverse(acc) }
    else { _map_acc(tl(lst), f, [f(hd(lst)) | acc]) }
}

fun filter(a, b) {
    if (typeof(a) == "list") { _filter_acc(a, b, []) }
    else { _filter_acc(b, a, []) }
}
fun _filter_acc(lst, pred, acc) {
    if (length(lst) == 0) { reverse(acc) }
    else {
        if (pred(hd(lst))) { _filter_acc(tl(lst), pred, [hd(lst) | acc]) }
        else { _filter_acc(tl(lst), pred, acc) }
    }
}

# Std.join(lst, sep) — alias for string_join (the name people guess).
fun join(lst, sep) { string_join(lst, sep) }

# string_join(lst, sep) -> "a,b,c"
fun string_join(lst, sep) {
    if (length(lst) == 0) { "" }
    else { if (length(lst) == 1) { to_string(hd(lst)) }
    else { to_string(hd(lst)) ++ sep ++ string_join(tl(lst), sep) } }
}

# string_pad_left(s, len, pad_char) -> "  hi" (right-align by left-padding)
fun string_pad_left(s, len, pad_ch) {
    diff = len - string_length(s)
    if (diff <= 0) { s }
    else { string_repeat(pad_ch, diff) ++ s }
}

# string_pad_right(s, len, pad_char) -> "hi  "
fun string_pad_right(s, len, pad_ch) {
    diff = len - string_length(s)
    if (diff <= 0) { s }
    else { s ++ string_repeat(pad_ch, diff) }
}

# string_repeat(s, n) -> s repeated n times.
fun string_repeat(s, n) { _str_rep_acc(s, n, "") }
fun _str_rep_acc(s, n, acc) {
    if (n <= 0) { acc }
    else { _str_rep_acc(s, n - 1, acc ++ s) }
}

# string_indent(s, n) — prefix every line of s with n spaces.
fun string_indent(s, n) {
    pad = string_repeat(" ", n)
    lines = string_split(s, "\n")
    string_join(_indent_lines(lines, pad, []), "\n")
}
fun _indent_lines(lines, pad, acc) {
    if (length(lines) == 0) { acc }
    else { _indent_lines(tl(lines), pad, list_append(acc, pad ++ hd(lines))) }
}

# ============================================================
# CONCURRENCY — bounded parallel fan-out
# ============================================================
#
# task_stream(list, fn, opts) — run fn over every item, AT MOST N at a
# time, and collect the results IN INPUT ORDER. This is the rate-limited
# swarm primitive: "run 100 LLM calls, 5 in flight" is
#
#     Std.task_stream(prompts, fun(p) { ask_llm(p) },
#                     %{max_concurrency: 5, timeout_ms: 30000})
#
# Each result is TAGGED so the caller can branch on every outcome —
# task_stream NEVER returns a bare nil (the bug `pmap` has, where a slow
# item silently becomes nil on its hardcoded 5s wall):
#
#     {'ok', value}            — fn(item) returned value
#     {'error', 'timeout'}     — fn(item) ran past timeout_ms (on_timeout: 'error')
#     {'error', reason}        — fn(item) crashed; reason is the DOWN reason
#
# opts (all optional):
#   max_concurrency : int  — max workers in flight at once (default: len(list))
#   timeout_ms      : int  — per-item deadline in ms (default: 0 = no timeout)
#   on_timeout      : atom — 'error' (default) records {'error','timeout'};
#                            'keep' leaves the slot open and waits forever
#                            (timeout disabled for that item)
#   on_result       : fn(index, item, cell) — called for every item as soon
#                            as its cell is known, in COMPLETION order, from
#                            the calling process. Timeouts and crashes reach
#                            it too, so a caller can persist every failure
#                            the moment it happens.
#
# Guarantee: each item's on_result runs exactly once, before task_stream
# returns, unless the OS process dies first. The cell it receives is the
# same one the returned list holds at that index.
#
# task_stream_each(list, fn, opts) takes the same opts and delivers cells
# only through on_result. It returns %{ok: n, timeout: n, error: n} counts
# and keeps no result list, so a very large run stays in constant memory.
#
# Implementation: pure sw over spawn_monitor + selective receive. The
# calling process is the coordinator. It keeps at most N workers alive,
# refilling a slot the instant one finishes. Crashes are caught via the
# {'DOWN', ref, ...} monitor message (a panicking fn becomes an
# {'error', reason} cell, not a hang). Timeouts are deadline-driven: the
# coordinator's `after` is the soonest in-flight deadline, so one slow
# item can't stall the others.
fun task_stream(lst, fn, opts) {
    on_res = _ts_on_result(opts)
    emit = fun(results, idx, item, cell) {
        on_res(idx, item, cell)
        map_put(results, idx, cell)
    }
    results = _ts_run(lst, fn, opts, %{}, emit)
    _ts_collect(0, length(lst), results, [])
}

fun task_stream_each(lst, fn, opts) {
    on_res = _ts_on_result(opts)
    emit = fun(counts, idx, item, cell) {
        on_res(idx, item, cell)
        _ts_count(counts, cell)
    }
    _ts_run(lst, fn, opts, %{ok: 0, timeout: 0, error: 0}, emit)
}

# on_result defaults to a no-op so the loop can always call it.
fun _ts_on_result(opts) {
    case map_get(opts, 'on_result') {
        nil -> fun(idx, item, cell) { 'ok' }
        f   -> f
    }
}

fun _ts_count(counts, cell) {
    kind = if (elem(cell, 0) == 'ok') { 'ok' }
           else { if (elem(cell, 1) == 'timeout') { 'timeout' } else { 'error' } }
    map_put(counts, kind, map_get(counts, kind) + 1)
}

# Run the stream. `acc` is threaded through `emit(acc, idx, item, cell)`,
# which returns the new accumulator. Returns the final accumulator.
fun _ts_run(lst, fn, opts, acc, emit) {
    n = length(lst)
    if (n == 0) { acc }
    else {
        max_c = case map_get(opts, 'max_concurrency') {
            nil -> n
            m   -> if (m < 1) { 1 } else { m }
        }
        tmo = case map_get(opts, 'timeout_ms') { nil -> 0 ; t -> t }
        on_tmo = case map_get(opts, 'on_timeout') { nil -> 'error' ; o -> o }
        coord = self()
        # Workers are closures over (fn, item) — no shared table. State is
        # threaded through the loop: `pending` holds the undispatched items
        # (in order, starting at index next_idx) and `left` counts the
        # cells not yet emitted.
        k = _ts_min(max_c, n)
        started = _ts_refill([], lst, 0, k, fn, tmo, coord)
        _ts_loop(elem(started, 0), elem(started, 1), elem(started, 2),
                 n, max_c, fn, tmo, on_tmo, coord, acc, emit)
    }
}

fun _ts_min(a, b) { if (a < b) { a } else { b } }

# Dispatch items from `pending` until `max_c` are in flight or none remain.
# Returns {in_flight, pending, next_idx}.
fun _ts_refill(in_flight, pending, next_idx, max_c, fn, tmo, coord) {
    if (length(in_flight) >= max_c || length(pending) == 0) { {in_flight, pending, next_idx} }
    else {
        spawned = _ts_spawn_one(next_idx, hd(pending), fn, tmo, coord)
        _ts_refill(list_append(in_flight, spawned), tl(pending), next_idx + 1, max_c, fn, tmo, coord)
    }
}

# Spawn one monitored worker for index `idx`. Returns {ref, pid, idx, deadline, item}.
# deadline is an absolute ms timestamp; 0 means "no timeout". The worker runs
# fn(item) inside a try so a panic becomes {'error', reason} (never a hang).
# item rides along so a timeout or crash can be reported with its item.
fun _ts_spawn_one(idx, item, fn, tmo, coord) {
    pair = spawn_monitor(fun() {
        tagged = try { {'ok', fn(item)} } catch e { {'error', e} }
        send(coord, {'task_done', idx, tagged})
    })
    deadline = if (tmo > 0) { timestamp() + tmo } else { 0 }
    {elem(pair, 1), elem(pair, 0), idx, deadline, item}
}

# Stop watching a finished worker and drop its DOWN if it is already queued,
# so task_stream never leaves monitor messages in the caller's mailbox.
fun _ts_forget(entry) {
    case entry {
        nil -> 'ok'
        e ->
            ref = elem(e, 0)
            demonitor(ref)
            receive {
                {'DOWN', r, _, _, _} when r == ref -> 'ok'
                after 0 -> 'ok'
            }
    }
}

# The coordinator loop. Returns the accumulator once all n cells are emitted.
#   in_flight : [{ref, pid, idx, deadline, item}, ...] currently-running workers
#   pending   : undispatched items, the first of which has index next_idx
#   left      : cells not yet emitted
#
# An item is emitted exactly once. A worker's in_flight entry is removed the
# moment its cell is emitted, so a message that arrives later for the same
# index (a task_done queued before a timeout kill, or a DOWN) finds no entry
# and is dropped. The in_flight entry is therefore the only dedupe state
# needed, and results never has to be kept just to be checked.
fun _ts_loop(in_flight, pending, next_idx, left, max_c, fn, tmo, on_tmo, coord, acc, emit) {
    if (left == 0) { acc }
    else {
        wait = _ts_wait_ms(in_flight, tmo, on_tmo)
        receive {
            {'task_done', idx, tagged} ->
                entry = _ts_find_idx(in_flight, idx)
                case entry {
                    nil ->
                        _ts_loop(in_flight, pending, next_idx, left, max_c, fn, tmo, on_tmo, coord, acc, emit)
                    e ->
                        _ts_forget(e)
                        emitted = emit(acc, idx, elem(e, 4), tagged)
                        rest = _ts_remove_idx(in_flight, idx, [])
                        refilled = _ts_refill(rest, pending, next_idx, max_c, fn, tmo, coord)
                        _ts_loop(elem(refilled, 0), elem(refilled, 1), elem(refilled, 2), left - 1, max_c,
                                 fn, tmo, on_tmo, coord, emitted, emit)
                }

            {'DOWN', ref, _, _, reason} ->
                # A worker died. If we already recorded its task_done the ref
                # is gone from in_flight (normal exit) — ignore. Otherwise it
                # crashed outside the try (or was killed); record the reason.
                entry = _ts_find_ref(in_flight, ref)
                case entry {
                    nil ->
                        _ts_loop(in_flight, pending, next_idx, left, max_c, fn, tmo, on_tmo, coord, acc, emit)
                    e ->
                        emitted = emit(acc, elem(e, 2), elem(e, 4), {'error', reason})
                        rest = _ts_remove_ref(in_flight, ref, [])
                        refilled = _ts_refill(rest, pending, next_idx, max_c, fn, tmo, coord)
                        _ts_loop(elem(refilled, 0), elem(refilled, 1), elem(refilled, 2), left - 1, max_c,
                                 fn, tmo, on_tmo, coord, emitted, emit)
                }

            after wait {
                # Deadline reached: time out every expired in-flight worker.
                now = timestamp()
                expired = _ts_expired(in_flight, now, [])
                emitted = _ts_timeout_each(expired, acc, emit)
                rest = _ts_keep_unexpired(in_flight, now, [])
                refilled = _ts_refill(rest, pending, next_idx, max_c, fn, tmo, coord)
                _ts_loop(elem(refilled, 0), elem(refilled, 1), elem(refilled, 2), left - length(expired), max_c,
                         fn, tmo, on_tmo, coord, emitted, emit)
            }
        }
    }
}

# How long to block on `receive`. With no timeout (tmo<=0) or on_timeout=='keep'
# we block indefinitely (a huge ms value). Otherwise it's the soonest deadline
# minus now, floored at 1 so an already-passed deadline fires immediately.
fun _ts_wait_ms(in_flight, tmo, on_tmo) {
    if (tmo <= 0 || on_tmo == 'keep') { 86400000 }
    else {
        soonest = _ts_soonest_deadline(in_flight, 0)
        if (soonest == 0) { 86400000 }
        else { rem = soonest - timestamp() ; if (rem < 1) { 1 } else { rem } }
    }
}

fun _ts_soonest_deadline(in_flight, best) {
    if (length(in_flight) == 0) { best }
    else {
        d = elem(hd(in_flight), 3)
        nb = if (d > 0 && (best == 0 || d < best)) { d } else { best }
        _ts_soonest_deadline(tl(in_flight), nb)
    }
}

# Emit {'error','timeout'} for each expired worker, kill it, and forget its
# monitor. Returns the updated accumulator.
fun _ts_timeout_each(expired, acc, emit) {
    if (length(expired) == 0) { acc }
    else {
        e = hd(expired)
        exit_proc(elem(e, 1), 'kill')
        _ts_forget(e)
        emitted = emit(acc, elem(e, 2), elem(e, 4), {'error', 'timeout'})
        _ts_timeout_each(tl(expired), emitted, emit)
    }
}

fun _ts_expired(in_flight, now, acc) {
    if (length(in_flight) == 0) { acc }
    else {
        e = hd(in_flight)
        d = elem(e, 3)
        na = if (d > 0 && now >= d) { list_append(acc, e) } else { acc }
        _ts_expired(tl(in_flight), now, na)
    }
}

fun _ts_keep_unexpired(in_flight, now, acc) {
    if (length(in_flight) == 0) { acc }
    else {
        e = hd(in_flight)
        d = elem(e, 3)
        na = if (d > 0 && now >= d) { acc } else { list_append(acc, e) }
        _ts_keep_unexpired(tl(in_flight), now, na)
    }
}

fun _ts_find_ref(in_flight, ref) {
    if (length(in_flight) == 0) { nil }
    else { if (elem(hd(in_flight), 0) == ref) { hd(in_flight) } else { _ts_find_ref(tl(in_flight), ref) } }
}

fun _ts_find_idx(in_flight, idx) {
    if (length(in_flight) == 0) { nil }
    else { if (elem(hd(in_flight), 2) == idx) { hd(in_flight) } else { _ts_find_idx(tl(in_flight), idx) } }
}

fun _ts_remove_ref(in_flight, ref, acc) {
    if (length(in_flight) == 0) { acc }
    else {
        e = hd(in_flight)
        na = if (elem(e, 0) == ref) { acc } else { list_append(acc, e) }
        _ts_remove_ref(tl(in_flight), ref, na)
    }
}

fun _ts_remove_idx(in_flight, idx, acc) {
    if (length(in_flight) == 0) { acc }
    else {
        e = hd(in_flight)
        na = if (elem(e, 2) == idx) { acc } else { list_append(acc, e) }
        _ts_remove_idx(tl(in_flight), idx, na)
    }
}

# Collect results 0..n-1 into an ordered list. Any gap (should never happen)
# defaults to {'error','missing'} so the return is total — every cell is a
# {'ok'|'error', _} tuple, never a bare nil.
fun _ts_collect(i, n, results, acc) {
    if (i >= n) { acc }
    else {
        cell = case map_get(results, i) { nil -> ({'error', 'missing'}) ; c -> c }
        _ts_collect(i + 1, n, results, list_append(acc, cell))
    }
}

# ============================================================
# STRUCTURED OUTPUT — decode-and-validate
# ============================================================

# json_expect(str, opts) — decode JSON and check shape in one step.
# Returns {'ok', map} only if str is a JSON object that contains every key
# in opts.required; otherwise {'error', reason}. This is the validate half
# of the instructor-style "schema -> validate -> retry" loop (Agent.ask_json
# does the retry). reason is human-readable so it can be fed back to the LLM.
#
#   case Std.json_expect(reply, %{required: ['name', 'age']}) {
#       {'ok', m}      -> use(m)
#       {'error', why} -> retry_with(why)
#   }
fun json_expect(str, opts) {
    decoded = json_decode(str)
    if (decoded == nil) { {'error', "invalid JSON"} }
    # A JSON object's source (after trimming) starts with '{'. We test the
    # string rather than the decoded value so this works identically in the
    # compiled and tree-walking (`swc run`) paths.
    else { if (string_starts_with(string_trim(str), "{") == 'false') { {'error', "expected a JSON object"} }
    else {
        required = case map_get(opts, 'required') { nil -> [] ; r -> r }
        missing = _je_missing(required, decoded, [])
        if (length(missing) == 0) { {'ok', decoded} }
        else { {'error', "missing required keys: " ++ string_join(missing, ", ")} }
    } }
}

fun _je_missing(required, m, acc) {
    if (length(required) == 0) { acc }
    else {
        k = hd(required)
        na = if (map_get(m, k) == nil) { list_append(acc, k) } else { acc }
        _je_missing(tl(required), m, na)
    }
}
