# gc_rss_probe.sw — RSS-bounded-ness probe for SwarmRT GC v1.
#
# INVARIANT under test: each process owns a per-process value arena that is
# freed wholesale on exit (process_destroy). A worker builds a pile of garbage
# (lists of maps of strings) in ITS arena, sends back only a tiny int result,
# then returns. After it exits its arena must be reclaimed.
#
# Therefore PEAK RSS must scale with CONCURRENT-LIVE workers (the wave width W),
# NOT with the cumulative number of spawns (TOTAL). We spawn in fixed-width
# waves and collect every reply in a wave before launching the next, which caps
# in-flight workers at W and actually exercises reclaim instead of just peak.
#
# Tunables: TOTAL spawns, W wave width, CHUNK garbage per worker. Bump TOTAL by
# 10x with W fixed: if peak RSS stays roughly flat, the GC reclaims. If peak RSS
# grows with TOTAL, arenas are leaking.

module Main

# ---- per-worker garbage builder --------------------------------------------
# Build a list of CHUNK maps, each carrying a few string fields, then fold it
# down to a single small int (a checksum of sizes). The intermediate list +
# maps + strings are pure garbage that lives only in this worker's arena.

fun junk_string(seed) {
    # a non-trivial string so each map actually allocates heap in the arena
    "node-" ++ to_string(seed) ++ "-" ++ to_string(seed * 7) ++ "-payloadpayloadpayload"
}

fun build_garbage(i, n, acc) {
    if (i >= n) { acc }
    else {
        m = map_put(map_new(), 'id', i)
        m2 = map_put(m, 'a', junk_string(i))
        m3 = map_put(m2, 'b', junk_string(i + 1))
        m4 = map_put(m3, 'c', junk_string(i + 2))
        build_garbage(i + 1, n, list_append(acc, m4))
    }
}

fun fold_sizes(lst, i, n, acc) {
    if (i >= n) { acc }
    else {
        m = elem_at(lst, i)
        fold_sizes(lst, i + 1, n, acc + string_length(map_get(m, 'a')))
    }
}

# elem_at: nth list item via hd/tl walk (lists are linked; no random index op)
fun elem_at(lst, i) {
    if (i <= 0) { hd(lst) }
    else { elem_at(tl(lst), i - 1) }
}

# A worker: allocate CHUNK garbage, reduce to one int, send {pid, checksum}
# back to parent, then RETURN (process exits -> arena freed).
fun worker(parent, id, chunk) {
    g = build_garbage(0, chunk, [])
    checksum = fold_sizes(g, 0, chunk, 0)
    send(parent, {'done', id, checksum})
}

# ---- parent: spawn waves, collect each wave fully before the next -----------

# Spawn `count` workers (returns immediately; they run concurrently).
fun spawn_wave(parent, base, count, chunk) {
    if (count <= 0) { 0 }
    else {
        spawn(worker(parent, base, chunk))
        spawn_wave(parent, base + 1, count - 1, chunk)
    }
}

# Block until `count` 'done' messages have arrived; sum their checksums so the
# optimizer can't elide the work.
fun collect(count, acc) {
    if (count <= 0) { acc }
    else {
        receive {
            {'done', _id, checksum} -> collect(count - 1, acc + checksum)
        }
    }
}

# Run waves until `done >= total`. Each wave is at most W workers; we fully
# drain it before the next, capping concurrent-live workers at W.
fun run_waves(done, total, wave_w, chunk, acc) {
    if (done >= total) { acc }
    else {
        remaining = total - done
        w = if (remaining < wave_w) { remaining } else { wave_w }
        spawn_wave(self(), done, w, chunk)
        acc2 = collect(w, acc)
        next_done = done + w
        # progress every ~20 waves so output stays light
        if ((next_done / wave_w) % 20 == 0) {
            print("progress " ++ to_string(next_done) ++ "/" ++ to_string(total) ++
                  " checksum=" ++ to_string(acc2))
        }
        run_waves(next_done, total, wave_w, chunk, acc2)
    }
}

fun main() {
    total  = 50000   # cumulative short-lived workers spawned
    wave_w = 200     # max concurrent-live workers (bounds peak RSS)
    chunk  = 200     # garbage maps each worker allocates then drops

    print("gc_rss_probe start total=" ++ to_string(total) ++
          " wave_w=" ++ to_string(wave_w) ++ " chunk=" ++ to_string(chunk))
    final = run_waves(0, total, wave_w, chunk, 0)
    print("gc_rss_probe DONE total=" ++ to_string(total) ++
          " final_checksum=" ++ to_string(final))
    print("PROBE_OK")
}
