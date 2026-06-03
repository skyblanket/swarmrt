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
    send(pid, 'incr')
    send(pid, 'incr')
    send(pid, 'incr')
    send(pid, 'incr')
    send(pid, 'incr')
    send(pid, {'get', self()})
    receive {
        n -> print(f"count={n}")
    }
}
