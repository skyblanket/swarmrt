module Distributed

fun echo_server() {
  register("echo", self())
  echo_loop()
}

fun echo_loop() {
  receive {
    {echo, from, msg} ->
      print("echo server got:", msg)
      send(from, {reply, msg})
      echo_loop()
    {stop} ->
      print("echo server stopping")
  }
}

fun main() {
  print("=== Distributed nodes test ===")

  ok = node_start("alpha@localhost", 19300)
  print("node_start:", ok)

  name = node_name()
  print("node_name:", name)

  peers = node_peers()
  print("peers (should be []):", peers)

  connected = node_is_connected("beta@localhost")
  print("beta connected?:", connected)

  print("node_stop")
  node_stop()

  print("=== Done ===")
}
