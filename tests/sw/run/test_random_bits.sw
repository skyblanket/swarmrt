module Test_random_bits

# random / random_seed (seedable xoshiro256**) && band/bor/bxor/bsl/bsr.
# Shared C helpers, so both backends agree.

fun check(name, ok) {
    if (ok) { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name) ; 1 }
}

fun draws(n, acc) {
    if (n == 0) { acc } else { draws(n - 1, acc ++ to_string(random(1000)) ++ ",") }
}

fun in_range(n, ok) {
    if (n == 0) { ok } else {
        r = random(10)
        in_range(n - 1, ok && r >= 0 && r < 10)
    }
}

fun main() {
    fails = 0
    random_seed(42)
    a = draws(20, "")
    random_seed(42)
    b = draws(20, "")
    fails = fails + check("seed_reproducible", a == b)
    fails = fails + check("range", in_range(200, true))
    f = random()
    fails = fails + check("float", f >= 0.0 && f < 1.0)
    fails = fails + check("nonpositive", random(0) == 0)
    fails = fails + check("band", band(12, 10) == 8)
    fails = fails + check("bor", bor(12, 10) == 14)
    fails = fails + check("bxor", bxor(12, 10) == 6)
    fails = fails + check("bsl", bsl(1, 4) == 16)
    fails = fails + check("bsr", bsr(256, 4) == 16)
    if (fails == 0) { print("OK random_bits") ; sys_exit(0) }
    else { print("FAIL random_bits") ; sys_exit(1) }
}
