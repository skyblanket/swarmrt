# msg_size_cap.sw — bidirectional gate for the LOCAL MESSAGE SIZE CAP
# (SW_MSG_MAX_BYTES, Phase 3 limits & quotas).
#
# The sender fires three messages at a receiver: a small one, a ~128 KB one,
# and a small end-marker. Under a 64 KB cap (the gate runs
# SW_MSG_MAX_BYTES=65536) the big send is DROPPED at sw_send_tagged_msg —
# loudly (the Makefile gate greps stderr for the SW_MSG_MAX_BYTES banner),
# leak-free (the message region is bulk-freed), and without wedging the
# receiver (the drop still wakes it — the receiver's next receive gets the
# marker, not a timeout). With the cap UNSET the same binary delivers the
# big message intact (full length verified) and stderr carries no banner —
# no false drops. Both runs print PROBE_OK.
#
# Bidirectional by construction: neuter the size check in sw_send_tagged_msg
# and the capped run FAILS (the big message arrives instead of the marker).
module Main

fun big_string(acc, n) {
    if (n <= 0) { acc }
    else { big_string(acc ++ "0123456789abcdef", n - 1) }
}

# Receiver: report the first two {'m', _} payloads back to the parent.
fun receiver(parent) {
    a = receive { {'m', x} -> x after 15000 { 'none' } }
    b = receive { {'m', x} -> x after 15000 { 'none' } }
    send(parent, {'got', a, b})
    0
}

fun capped_check(a, b) {
    # Cap 65536: small delivered, big dropped, marker delivered NEXT —
    # and promptly (a drop that failed to wake the receiver would park it
    # until the 15s receive timeout returns 'none' instead of the marker).
    b_ok = case b { 'marker' -> 1  _ -> 0 }
    if (a == 'small_one' && b_ok == 1) {
        print("msg_size_cap capped: big dropped, marker delivered, receiver woken")
        print("PROBE_OK")
        sys_exit(0)
    } else {
        print(f"FAIL msg_size_cap capped: a={a} b={b}")
        sys_exit(1)
    }
}

fun uncapped_check(a, b) {
    # No cap: the big message must arrive INTACT (full length) as message 2.
    blen = case b {
        {'big', s} -> string_length(s)
        _          -> 0
    }
    if (a == 'small_one' && blen == 131073) {
        print("msg_size_cap uncapped: big delivered intact (131073 bytes)")
        print("PROBE_OK")
        sys_exit(0)
    } else {
        print(f"FAIL msg_size_cap uncapped: a={a} blen={blen}")
        sys_exit(1)
    }
}

fun main() {
    me = self()
    r = spawn(receiver(me))
    send(r, {'m', 'small_one'})
    big = big_string("b", 8192)          # 1 + 16*8192 = 131073 bytes
    send(r, {'m', {'big', big}})
    send(r, {'m', 'marker'})
    got = receive { {'got', a, b} -> {a, b} after 30000 { {'none', 'none'} } }
    a = elem(got, 0)
    b = elem(got, 1)
    q = getenv("SW_MSG_MAX_BYTES")
    case q {
        'nil' -> uncapped_check(a, b)
        "0"   -> uncapped_check(a, b)
        _     -> capped_check(a, b)
    }
}
