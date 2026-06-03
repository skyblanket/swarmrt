module Main

fun counter(n) {
    receive {
        'incr' -> counter(n + 1)
        {'get', caller} -> {
            send(caller, n)
            counter(n)
        }
    }
}

fun main() {
    pid = spawn(fun() { counter(0) })
    for _ in [1, 2, 3, 4, 5] {
        send(pid, 'incr')
    }
    send(pid, {'get', self()})
    receive {
        count -> print(f"count={count}")
    }
}
