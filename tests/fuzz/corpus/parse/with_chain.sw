module W
export [main]

fun step(v) {
    if (v < 0) { {'err', v} } else { {'ok', v * 2} }
}

fun pipe(a, b) {
    with {'ok', x} <- step(a),
         {'ok', y} <- step(b),
         %{status: 200} <- check(x + y) {
        {'ok', x + y}
    } else {
        other -> {'failed', other}
    }
}

fun one(v) {
    with {'ok', n} <- step(v) { n } else { e -> e }
}

fun check(n) {
    if (n > 0) { %{status: 200} } else { %{status: 500} }
}

fun main() {
    print(pipe(1, 2))
    print(one(3))
}
