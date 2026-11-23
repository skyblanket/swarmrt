module Test_random_bits

# #166: random() / random(n) / random_seed(n) (xoshiro256**, one stream per
# scheduler), band/bor/bxor/bnot/bsl/bsr, and string_repeat. One shared C
# helper, so both backends agree (compiled here + the tests/sw/run/ copy via
# swc run) down to the seeded stream. Bad arguments panic; they never return
# a quiet 0 or "". Ported from O's #181, with its checks kept.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun draws(n, acc) {
    if (n == 0) { acc } else { draws(n - 1, acc ++ to_string(random(1000)) ++ ",") }
}

fun in_range(n, k, ok) {
    if (n == 0) { ok } else {
        r = random(k)
        in_range(n - 1, k, ok && typeof(r) == "int" && r >= 0 && r < k)
    }
}

fun floats_ok(n, ok, sum) {
    if (n == 0) { {ok, sum} } else {
        f = random()
        floats_ok(n - 1, ok && typeof(f) == "float" && f >= 0.0 && f < 1.0, sum + f)
    }
}

# How many of n draws of random(k) equal v.
fun count_hits(n, k, v, c) {
    if (n == 0) { c } else {
        hit = if (random(k) == v) { 1 } else { 0 }
        count_hits(n - 1, k, v, c + hit)
    }
}

fun test_random() {
    fails = 0
    random_seed(42)
    a = draws(20, "")
    random_seed(42)
    b = draws(20, "")
    fails = fails + assert_eq("seed_reproducible", a == b, true)
    random_seed(43)
    fails = fails + assert_eq("seed_differs", draws(20, "") == a, false)
    # The same seed gives the same stream in both backends.
    random_seed(42)
    fails = fails + assert_eq("seed_42_stream", draws(3, ""), "742,102,9,")
    fails = fails + assert_eq("seed_returns_ok", random_seed(7), 'ok')
    fails = fails + assert_eq("range_10", in_range(2000, 10, true), true)
    fails = fails + assert_eq("range_1", in_range(50, 1, true), true)
    fails = fails + assert_eq("range_big", in_range(200, 9000000000000000000, true), true)
    r = floats_ok(2000, true, 0.0)
    fails = fails + assert_eq("float_range", elem(r, 0), true)
    mean = elem(r, 1) / 2000.0
    fails = fails + assert_eq("float_mean", mean > 0.45 && mean < 0.55, true)
    # Each of 10 buckets gets ~1000 of 10000 draws; 800..1200 is > 6 sigma.
    hits = count_hits(10000, 10, 3, 0)
    fails = fails + assert_eq("uniform_bucket", hits > 800 && hits < 1200, true)
    fails
}

fun test_bits() {
    fails = 0
    fails = fails + assert_eq("band", band(12, 10), 8)
    fails = fails + assert_eq("bor", bor(12, 10), 14)
    fails = fails + assert_eq("bxor", bxor(12, 10), 6)
    fails = fails + assert_eq("bnot", bnot(0), -1)
    fails = fails + assert_eq("bnot_5", bnot(5), -6)
    fails = fails + assert_eq("bsl", bsl(1, 4), 16)
    fails = fails + assert_eq("bsl_63", bsl(1, 63) < 0, true)
    fails = fails + assert_eq("bsr", bsr(256, 4), 16)
    fails = fails + assert_eq("bsr_neg", bsr(-256, 4), -16)
    fails = fails + assert_eq("bsr_0", bsr(5, 0), 5)
    fails = fails + assert_eq("band_neg", band(-1, 255), 255)
    fails
}

fun test_repeat() {
    fails = 0
    fails = fails + assert_eq("repeat", string_repeat("ab", 3), "ababab")
    fails = fails + assert_eq("repeat_once", string_repeat("ab", 1), "ab")
    fails = fails + assert_eq("repeat_zero", string_repeat("ab", 0), "")
    fails = fails + assert_eq("repeat_neg", string_repeat("ab", -2), "")
    fails = fails + assert_eq("repeat_empty", string_repeat("", 5), "")
    fails = fails + assert_eq("repeat_utf8", string_repeat("—", 2), "——")
    fails = fails + assert_eq("repeat_len", string_length(string_repeat("xyz", 10000)), 30000)
    fails
}

# Bad arguments panic with a clear message (seen in the DOWN reason).
fun bad_random_zero() { random(0) }
fun bad_random_neg() { random(-5) }
fun bad_random_float() { random(2.5) }
fun bad_seed() { random_seed("x") }
fun bad_band() { band(1, "2") }
fun bad_shift() { bsl(1, 64) }
fun bad_shift_neg() { bsr(1, -1) }
fun bad_repeat() { string_repeat(5, 2) }

fun panics_with(name, pid_ref, want) {
    receive {
        {'DOWN', _, _, _, why} ->
            assert_eq(name, string_contains(to_string(why), want), true)
        after 3000 { assert_eq(name, "TIMEOUT", want) }
    }
}

fun test_bad_args() {
    fails = 0
    fails = fails + panics_with("bad_random_zero", spawn_monitor(bad_random_zero()), "random(n): n must be a positive int, got 0")
    fails = fails + panics_with("bad_random_neg", spawn_monitor(bad_random_neg()), "got -5")
    fails = fails + panics_with("bad_random_float", spawn_monitor(bad_random_float()), "got a float")
    fails = fails + panics_with("bad_seed", spawn_monitor(bad_seed()), "random_seed(n): n must be an int")
    fails = fails + panics_with("bad_band", spawn_monitor(bad_band()), "band: expected int, int")
    fails = fails + panics_with("bad_shift", spawn_monitor(bad_shift()), "bsl: shift count must be 0..63, got 64")
    fails = fails + panics_with("bad_shift_neg", spawn_monitor(bad_shift_neg()), "bsr: shift count must be 0..63")
    fails = fails + panics_with("bad_repeat", spawn_monitor(bad_repeat()), "string_repeat: expected (string, int)")
    fails
}

fun main() {
    fails = test_random() + test_bits() + test_repeat() + test_bad_args()
    if (fails == 0) { print("OK random_bits") ; sys_exit(0) }
    else { print("FAIL random_bits") ; sys_exit(1) }
}
