# slope_message.sw — memory-slope probe for ESCAPED VALUE MESSAGES (Ownership v2).
#
# A high-volume message-passing service: a sender pushes N large (~32 KB) messages
# to a long-lived receiver at FIXED mailbox depth (depth 1 via an ack handshake).
# Under GC v1 every message payload is deep-copied to the global heap on send and
# never reclaimed, and the long-lived receiver/sender loops never reclaim per-turn
# garbage — so peak RSS grows with CUMULATIVE messages even though depth is fixed.
# After Ownership v2 (message-owned regions freed on consume + per-turn loop reset)
# the post-warmup slope must be near-flat.
#
# Usage: slope_message <messages>   (default 4000). Driven by scripts/gc_slope.sh.

module Main

fun big_string(acc, n) {
    if (n <= 0) { acc }
    else { big_string(acc ++ "0123456789abcdef0123456789abcdef", n - 1) }
}

# Long-lived receiver: consume each message, ack the sender, recurse (tail-call).
fun receiver() {
    receive {
        {'data', payload, from} -> {
            _sz = string_length(payload)   # force the payload live, then discard
            send(from, 'ack')
            receiver()
        }
        'fin' -> 'done'
    }
}

# Sender loop (run by main): depth-1 — send one, wait for ack, send next.
fun send_loop(rcv, me, big, n) {
    if (n <= 0) { 0 }
    else {
        send(rcv, {'data', big, me})
        receive { 'ack' -> send_loop(rcv, me, big, n - 1) }
    }
}

fun job_count(args) {
    if (length(args) >= 2) { to_int(hd(tl(args))) } else { 4000 }
}

fun main() {
    msgs = job_count(os_args())
    big  = big_string("", 1024)   # ~32 KB, built once
    print("slope_message messages=" ++ to_string(msgs) ++
          " depth=1 msg_bytes=" ++ to_string(string_length(big)))
    rcv = spawn(receiver())
    send_loop(rcv, self(), big, msgs)
    send(rcv, 'fin')
    print("slope_message DONE messages=" ++ to_string(msgs))
    print("PROBE_OK")
    0
}
