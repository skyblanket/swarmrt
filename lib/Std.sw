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
    # ---- map ops ----
    map_each, map_filter,
    # ---- string ops ----
    string_join, string_pad_left, string_pad_right, string_repeat,
    string_indent
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

# sum(lst) — assumes numeric elements.
fun sum(lst) { reduce(fun(acc, x) { acc + x }, lst, 0) }

# product(lst) — assumes numeric elements.
fun product(lst) { reduce(fun(acc, x) { acc * x }, lst, 1) }

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
