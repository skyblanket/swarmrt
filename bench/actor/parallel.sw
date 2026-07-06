module Main
fun consumer(n) { if (n <= 0) { 0 } else { receive { 'm' -> 0 } consumer(n - 1) } }
fun producer(c, n, parent) { if (n <= 0) { send(parent, 'fin') 0 } else { send(c, 'm') producer(c, n - 1, parent) } }
fun spawn_pairs(k, m, parent) {
    if (k <= 0) { 0 }
    else { c = spawn(consumer(m)) ; spawn(producer(c, m, parent)) ; spawn_pairs(k - 1, m, parent) }
}
fun wait_fins(k) { if (k <= 0) { 0 } else { receive { 'fin' -> 0 } wait_fins(k - 1) } }
fun main() { k = 20 ; m = 500000 ; spawn_pairs(k, m, self()) ; wait_fins(k) ; print("done") }
