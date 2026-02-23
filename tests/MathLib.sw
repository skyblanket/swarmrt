module MathLib

fun square(x) {
    x * x
}

fun double(x) {
    x + x
}

fun clamp(val, lo = 0, hi = 100) {
    if (val < lo) { lo }
    elsif (val > hi) { hi }
    else { val }
}
