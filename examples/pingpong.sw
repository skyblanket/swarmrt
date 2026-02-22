# Ping-pong between two processes
# Demonstrates bidirectional message passing

module PingPong

export [main, pong]

fun pong(n) {
    receive {
        {'ping', from} ->
            print("pong " ++ n)
            send(from, {'pong', n})
            pong(n + 1)

        'stop' ->
            print("pong done after " ++ n ++ " rounds")
    }
}

fun main() {
    ponger = spawn(pong(0))

    send(ponger, {'ping', self()})
    receive { {'pong', n} -> print("got pong " ++ n) }

    send(ponger, {'ping', self()})
    receive { {'pong', n} -> print("got pong " ++ n) }

    send(ponger, {'ping', self()})
    receive { {'pong', n} -> print("got pong " ++ n) }

    send(ponger, 'stop')
}
