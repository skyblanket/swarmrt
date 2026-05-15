# dispatcher.sw — actor that routes tagged messages with `case`.
#
# Demonstrates the studio pattern from swarm-code: a long-lived process
# that receives tagged tuples and dispatches via case + format. State is
# carried as the recursion arg.

module Dispatcher
export [main, agent]

fun agent(name, stats) {
    receive {
        msg -> handle(name, msg, stats)
    }
}

fun handle(name, msg, stats) {
    new_stats = case msg {
        {'request', verb, payload, from} ->
            send(from, {'reply', f"[{name}] {verb} got: {payload}"})
            map_put(stats, 'requests', map_get(stats, 'requests') + 1)

        {'health', from} ->
            n = map_get(stats, 'requests')
            send(from, {'health', f"{name} ok — {n} request(s) handled"})
            stats

        'stop' ->
            print(f"[{name}] stopping after {map_get(stats, 'requests')} request(s)")
            'done'

        other ->
            send(self(), {'unknown', other})
            stats
    }
    case new_stats {
        'done' -> 'ok'
        _      -> agent(name, new_stats)
    }
}

fun main() {
    a = spawn(agent("scout", %{requests: 0}))

    send(a, {'health', self()})
    receive { {'health', txt} -> print(txt) }

    send(a, {'request', "ping", "first call", self()})
    send(a, {'request', "search", "hello world", self()})
    receive { {'reply', txt} -> print(txt) }
    receive { {'reply', txt} -> print(txt) }

    send(a, {'health', self()})
    receive { {'health', txt} -> print(txt) }

    send(a, 'stop')
    sleep(50)
}
