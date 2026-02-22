# Counter with process spawning
# Shows spawn, send, receive patterns

module Counter

export [main, counter]

# Counter process that maintains state
fun counter(start) {
    receive {
        {'increment', by} ->
            counter(start + by)

        {'get', from} ->
            send(from, {'count', start})
            counter(start)

        'stop' ->
            print("Counter stopped at " ++ start)
    }
}

fun main() {
    # Spawn counter with initial value 0
    pid = spawn(counter(0))

    # Send some increments
    send(pid, {'increment', 5})
    send(pid, {'increment', 3})
    send(pid, {'get', self()})

    # Receive result
    receive {
        {'count', n} -> print("Count: " ++ n)
    }

    send(pid, 'stop')
}
