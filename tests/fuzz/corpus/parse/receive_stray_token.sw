module Counter
fun counter(start) {
    receive {
        D {'count', n} -> print("Count")
    }
}
