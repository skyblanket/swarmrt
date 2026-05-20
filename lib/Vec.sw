# Vec.sw — tiny vector store with cosine-similarity search.
#
# Designed for agent memory at small-to-medium scale (up to ~100K
# vectors). O(N) per query — fine until you need approximate-NN.
# Backed by ETS so multiple processes can share the same store.
#
#   v = Vec.new()
#   Vec.add(v, "doc-1", [0.1, 0.2, ...], %{text: "hello"})
#   results = Vec.search(v, query_vec, 5)   # → [{id, score, payload}, ...]
#   Vec.size(v)
#   Vec.delete(v, "doc-1")
#
# Combine with Embed.create() for the full retrieval-augmented loop:
#   q_vec = Embed.create(opts, user_question)
#   hits  = Vec.search(store, q_vec, 5)
#   context = Std.string_join(map(fun(h) { map_get(elem(h, 2), 'text') }, hits), "\n---\n")

module Vec
export [new, add, delete, size, search, clear]

# Each entry is stored under its own id-key. We also keep a special
# '__ids__' key holding the list of all ids so search can iterate.
fun new() { ets_new() }

fun add(store, id, vec, payload) {
    entry = %{id: id, vec: vec, payload: payload}
    ets_put(store, id, entry)
    ids = case ets_get(store, '__ids__') {
        nil -> [id]
        existing -> if (contains(existing, id) == 'true') { existing } else { list_append(existing, id) }
    }
    ets_put(store, '__ids__', ids)
    'ok'
}

fun delete(store, id) {
    ets_delete(store, id)
    case ets_get(store, '__ids__') {
        nil -> 'ok'
        ids -> ets_put(store, '__ids__', remove_id(ids, id))
    }
}

fun clear(store) {
    case ets_get(store, '__ids__') {
        nil -> 'ok'
        ids -> clear_loop(store, ids)
    }
    ets_put(store, '__ids__', [])
}

fun clear_loop(store, ids) {
    if (length(ids) == 0) { 'ok' }
    else { ets_delete(store, hd(ids)) ; clear_loop(store, tl(ids)) }
}

fun size(store) {
    case ets_get(store, '__ids__') {
        nil -> 0
        ids -> length(ids)
    }
}

# search(store, query_vec, k) → list of {id, score, payload}, top-k by
# cosine similarity, highest first.
fun search(store, q_vec, k) {
    ids = case ets_get(store, '__ids__') { nil -> [] ; xs -> xs }
    scored = score_all(store, q_vec, ids, [])
    top_k(scored, k)
}

fun score_all(store, q_vec, ids, acc) {
    if (length(ids) == 0) { acc }
    else {
        id = hd(ids)
        entry = ets_get(store, id)
        if (entry == nil) { score_all(store, q_vec, tl(ids), acc) }
        else {
            score = cosine(q_vec, map_get(entry, 'vec'))
            score_all(store, q_vec, tl(ids),
                list_append(acc, {id, score, map_get(entry, 'payload')}))
        }
    }
}

# Insertion-sort the top-K — fine for k up to ~50.
fun top_k(scored, k) { top_k_loop(scored, k, []) }
fun top_k_loop(scored, k, sorted) {
    if (length(scored) == 0) { take_n(sorted, k) }
    else { top_k_loop(tl(scored), k, insert_desc(hd(scored), sorted, [])) }
}
fun insert_desc(item, sorted, acc) {
    if (length(sorted) == 0) { list_append(acc, item) }
    else {
        h = hd(sorted)
        if (elem(item, 1) > elem(h, 1)) { acc ++ [item] ++ sorted }
        else { insert_desc(item, tl(sorted), list_append(acc, h)) }
    }
}
fun take_n(lst, n) {
    if (n <= 0 || length(lst) == 0) { [] }
    else { [hd(lst)] ++ take_n(tl(lst), n - 1) }
}

# Cosine similarity. Assumes equal-length vectors; doesn't panic if
# they differ (just truncates to the shorter one).
fun cosine(a, b) {
    dp = dot(a, b)
    na = sqrt_approx(dot(a, a))
    nb = sqrt_approx(dot(b, b))
    if (na == 0 || nb == 0) { 0 }
    else { dp / (na * nb) }
}

fun dot(a, b) {
    if (length(a) == 0 || length(b) == 0) { 0 }
    else { hd(a) * hd(b) + dot(tl(a), tl(b)) }
}

# Newton's method sqrt — sw doesn't have a math builtin. 8 iterations
# is enough for the magnitudes typical embedding vectors produce.
fun sqrt_approx(x) {
    if (x <= 0) { 0 }
    else { sqrt_iter(x, x / 2, 8) }
}
fun sqrt_iter(x, guess, n) {
    if (n <= 0) { guess }
    else { sqrt_iter(x, (guess + x / guess) / 2, n - 1) }
}

# ---- helpers ----

fun contains(lst, v) {
    if (length(lst) == 0) { 'false' }
    else { if (hd(lst) == v) { 'true' } else { contains(tl(lst), v) } }
}

fun remove_id(lst, v) { remove_loop([], lst, v) }
fun remove_loop(acc, rest, v) {
    if (length(rest) == 0) { acc }
    else { if (hd(rest) == v) { acc ++ tl(rest) }
    else { remove_loop(list_append(acc, hd(rest)), tl(rest), v) }}
}
