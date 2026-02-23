module Supervisor

fun worker(name, count) {
  receive {
    {ping, from} ->
      print(name, "pong", count)
      send(from, {pong, count})
      worker(name, count + 1)
    {stop} ->
      print(name, "stopping at", count)
  }
}

fun main() {
  sup = supervise('one_for_one', [
    {'worker_a', fun() { worker("A", 0) }, 'permanent'},
    {'worker_b', fun() { worker("B", 0) }, 'permanent'}
  ])
  print("Supervisor started:", sup)

  sleep(500)

  a = whereis("worker_a")
  b = whereis("worker_b")
  print("worker_a:", a)
  print("worker_b:", b)

  send(a, {ping, self()})
  receive {
    {pong, n} -> print("Got pong from A:", n)
  }

  send(b, {ping, self()})
  receive {
    {pong, n} -> print("Got pong from B:", n)
  }

  sleep(200)
  print("Supervisor test done")
}
