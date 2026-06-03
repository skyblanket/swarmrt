module Main

fun counter(value) {
    receive {
        'incr' -> counter(value + 1)
        {'get', caller} -> {
            send(caller, value)
            counter(value)
        }
    }
}

fun main() {
    pid = spawn(fun() { counter(0) })
    for _ in [1, 2, 3, 4, 5] { send(pid, 'incr') }
    send(pid, {'get', self()})
    receive {
        v -> print(f"count={v}")
    }
}
