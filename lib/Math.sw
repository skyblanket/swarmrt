# Math.sw — sw math standard library.
#
# Thin wrappers over the libm-backed C builtins (math_sqrt, math_sin, …)
# so callers write the idiomatic Math.sqrt(x) instead of the bare
# builtin, plus a few pure-sw helpers (min / max / clamp / pi) that need
# no C support.
#
# Auto-discoverable: `import Math` works anywhere — swc falls back to
# <swarmrt-root>/lib/ after the local directory.
#
# All trig is in radians. sqrt/sin/cos/pow/exp/log return floats;
# floor/ceil/round return ints. Each accepts an int OR a float.

module Math

export [
    sqrt, sin, cos, pow, exp, log,
    floor, ceil, round,
    float,
    pi, min, max, clamp
]

# ---- libm-backed (C builtins) ----

fun sqrt(x)   { math_sqrt(x) }
fun sin(x)    { math_sin(x) }
fun cos(x)    { math_cos(x) }
fun pow(b, e) { math_pow(b, e) }
fun exp(x)    { math_exp(x) }
fun log(x)    { math_log(x) }
fun floor(x)  { math_floor(x) }
fun ceil(x)   { math_ceil(x) }
fun round(x)  { math_round(x) }

# ---- conversions ----
# `abs` is already a global core builtin — use it directly, no Math.abs
# needed. `float` here is a friendlier alias for the to_float builtin.

fun float(x) { to_float(x) }

# ---- pure-sw helpers ----

# pi to double precision.
fun pi() { 3.141592653589793 }

fun min(a, b) { if (a < b) { a } else { b } }
fun max(a, b) { if (a > b) { a } else { b } }

# clamp(x, lo, hi) — confine x to [lo, hi].
fun clamp(x, lo, hi) {
    if (x < lo) { lo }
    else { if (x > hi) { hi } else { x } }
}
