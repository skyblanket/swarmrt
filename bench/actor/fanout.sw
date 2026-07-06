module Main
fun sink(n) {
    if (n <= 0) { 0 }
    else { receive { 'm' -> 0 } sink(n - 1) }
}
fun send_n(p, n) { if (n <= 0) { 0 } else { send(p, 'm') ; send_n(p, n - 1) } }
fun main() {
    n = 5000000                # 5M messages one-way
    p = spawn(sink(n))
    send_n(p, n)
    print("done")
}
