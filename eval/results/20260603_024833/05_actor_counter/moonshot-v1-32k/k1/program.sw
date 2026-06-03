module Main

fun counter(value) {
    receive {
        {'incr', caller} -> {
            send(caller, value)
            counter(value + 1)
        }
        {'get', caller} -> {
            send(caller, value)
            counter(value)
        }
    }
}

fun main() {
    counter_pid = spawn(fun() { counter(0) })
    for _ in [1, 2, 3, 4, 5] { send(counter_pid, {'incr', self()}) }
    send(counter_pid, {'get', self()})
    receive {
        v -> print(f"count={v}")
    }
}
