module Main

fun counter(n) {
    receive {
        'incr'       -> counter(n + 1)
        {'get', pid} -> {
            send(pid, n)
            counter(n)
        }
    }
}

fun main() {
    pid = spawn(fun() { counter(0) })
    for _ in [1,2,3,4,5] {
        send(pid, 'incr')
    }
    send(pid, {'get', self()})
    receive {
        total -> print(f"count={total}")
    }
}
