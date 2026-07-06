module Main
# pong: reply to each ping with the sender; loop
fun pong() {
    receive {
        {'ping', from} -> send(from, 'pong') ; pong()
        'stop' -> 0
    }
}
fun ping_loop(p, n) {
    if (n <= 0) { send(p, 'stop') 0 }
    else {
        send(p, {'ping', self()})
        receive { 'pong' -> 0 }
        ping_loop(p, n - 1)
    }
}
fun main() {
    p = spawn(pong())
    ping_loop(p, 2000000)   # 2M round-trips
    print("done")
}
