# slope_spawn.sw — memory-slope probe for ESCAPED SPAWN ARGUMENTS (Ownership v2).
#
# The review's repro: spawn workers at FIXED concurrency, each with the SAME large
# (~32 KB) argument. Under GC v1 that argument is deep-copied to the global heap on
# every spawn and never reclaimed, so peak RSS grows with CUMULATIVE jobs even though
# concurrency is fixed. After Ownership v2 (spawn-owned regions adopted by the child
# and freed on child exit) the post-warmup slope must be near-flat.
#
# Usage: slope_spawn <jobs>   (default 2000). Concurrency is fixed at W below.
# Driven by scripts/gc_slope.sh, which runs it at a low and a high job count and
# fails if peak-RSS growth exceeds a budget.

module Main

# Build a ~32 KB string once (1024 * 32 bytes). This is the fixed spawn argument.
fun big_string(acc, n) {
    if (n <= 0) { acc }
    else { big_string(acc ++ "0123456789abcdef0123456789abcdef", n - 1) }
}

# Worker: read the argument (force it to be live), report its size, then EXIT.
fun worker(arg, parent) {
    send(parent, {'done', string_length(arg)})
}

fun spawn_wave(arg, parent, count) {
    if (count <= 0) { 0 }
    else { spawn(worker(arg, parent)) ; spawn_wave(arg, parent, count - 1) }
}

fun collect(count, acc) {
    if (count <= 0) { acc }
    else { receive { {'done', _sz} -> collect(count - 1, acc + 1) } }
}

# Run `total` jobs in waves of at most W, draining each wave fully (caps
# concurrent-live workers at W so peak RSS tests reclaim, not just headroom).
fun run_waves(arg, done, total, w, acc) {
    if (done >= total) { acc }
    else {
        rem = total - done
        ww = if (rem < w) { rem } else { w }
        spawn_wave(arg, self(), ww)
        acc2 = collect(ww, acc)
        run_waves(arg, done + ww, total, w, acc2)
    }
}

fun job_count(args) {
    if (length(args) >= 2) { to_int(hd(tl(args))) } else { 2000 }
}

fun main() {
    jobs = job_count(os_args())
    w    = 50                  # fixed concurrency
    arg  = big_string("", 1024)   # ~32 KB
    print("slope_spawn jobs=" ++ to_string(jobs) ++ " concurrency=" ++ to_string(w) ++
          " arg_bytes=" ++ to_string(string_length(arg)))
    done = run_waves(arg, 0, jobs, w, 0)
    print("slope_spawn DONE jobs=" ++ to_string(done))
    print("PROBE_OK")
    0
}
