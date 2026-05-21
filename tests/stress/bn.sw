module Bisect
export [main]

fun child(parent) { send(parent, 'done') ; 'ok' }

fun spawn_n(n) {
    if (n == 0) { 'ok' }
    else { spawn(child(self())) ; spawn_n(n - 1) }
}

fun await_n(n) {
    if (n == 0) { 'ok' }
    else { receive { 'done' -> await_n(n - 1) } }
}

fun main() {
    n = 80000
    spawn_n(n); await_n(n)
    print(f"ok {n}")
}
