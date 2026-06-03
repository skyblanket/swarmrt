module Main

fun counter(count) {
    receive {
        'incr' -> counter(count + 1)
        {'get', caller} ->
            send(caller, count)
            counter(count)
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
        final_count -> print(f"count={final_count}")
    }
}
