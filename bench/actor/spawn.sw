module Main
fun worker() { 0 }
fun spawn_n(n) { if (n <= 0) { 0 } else { spawn(worker()) ; spawn_n(n - 1) } }
fun main() { spawn_n(1000000) ; print("done") }
