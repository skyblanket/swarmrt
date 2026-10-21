module Test_mutual_tail

# Mutual tail calls between module functions run in constant C stack. Before
# the hop codegen, `loop -> step -> loop` overflowed the 128KB fiber stack
# after a few thousand turns, and a receive loop whose handler called the
# loop again died the same way under load (swarm-live, 2026-10).

fun loop(n) { if (n == 0) { 'done' } else { step(n) } }
fun step(n) { loop(n - 1) }

# Arguments cross a hop by value: a swap must not turn into a duplicate.
fun ping(a, b, n) { if (n == 0) { [a, b] } else { pong(b, a, n - 1) } }
fun pong(a, b, n) { ping(b, a, n) }

# Tail call from inside a case arm.
fun classify(x) {
    case x {
        0 -> 'zero'
        _ -> classify_step(x - 1)
    }
}
fun classify_step(x) { classify(x) }

# Receive loop whose handler calls the loop again: the canonical server shape.
fun server(state, parent) {
    receive {
        'msg' -> handle(state, parent)
        'stop' -> send(parent, {'served', state})
    }
}
fun handle(state, parent) { server(state + 1, parent) }

fun send_n(pid, n) {
    if (n == 0) { send(pid, 'stop') } else { send(pid, 'msg')
        send_n(pid, n - 1) }
}

# Non-tail recursion through a module function still nests and returns.
fun depth(n) { if (n == 0) { 0 } else { 1 + depth(n - 1) } }
fun mixed(n) { if (n == 0) { depth(100) } else { mixed_b(n - 1) } }
fun mixed_b(n) { mixed(n) }

# User locals named like codegen-internal names (`_fl` was the hop's arena
# floor, `_sw_tc_fl` is now, `_args` holds the arguments) must still compile:
# generated names can't collide with user ones.
fun reserved_a(n) { _fl = n * 2 ; _sw_tc_fl = _fl ; _args = _sw_tc_fl ; if (n == 0) { _args } else { reserved_b(n - 1) } }
fun reserved_b(n) { reserved_a(n) }

fun main() {
    failures = 0

    # --- 1. the repro: 5,000,000 mutual turns in flat stack
    r = loop(5000000)
    failures = failures + (if (r == 'done') { print("PASS mutual_loop_flat") ; 0 }
                           else { print("FAIL mutual_loop_flat"); 1 })

    # --- 2. argument order is kept across hops (even and odd hop counts)
    failures = failures + (if (ping(1, 2, 4) == [1, 2]) { print("PASS swap_even") ; 0 }
                           else { print("FAIL swap_even"); 1 })
    failures = failures + (if (ping(1, 2, 3) == [1, 2]) { print("PASS swap_odd") ; 0 }
                           else { print("FAIL swap_odd"); 1 })

    # --- 3. tail call from a case arm
    failures = failures + (if (classify(300001) == 'zero') { print("PASS case_arm") ; 0 }
                           else { print("FAIL case_arm"); 1 })

    # --- 4. receive loop -> handler -> receive loop, 200k messages
    me = self()
    srv = spawn(server(0, me))
    spawn(send_n(srv, 200000))
    served = receive {
        {'served', n} -> n
        after 30000 { 'timeout' }
    }
    failures = failures + (if (served == 200000) { print("PASS server_loop") ; 0 }
                           else { print("FAIL server_loop got=" ++ to_string(served)) ; 1 })

    # --- 5. non-tail recursion mixed with hops
    failures = failures + (if (mixed(100001) == 100) { print("PASS non_tail_mixed") ; 0 }
                           else { print("FAIL non_tail_mixed"); 1 })

    # --- 6. a user local named like a generated parameter
    failures = failures + (if (reserved_a(1000) == 0) { print("PASS reserved_local_name") ; 0 }
                           else { print("FAIL reserved_local_name"); 1 })

    if (failures == 0) { print("OK test_mutual_tail 6/6") ; sys_exit(0) }
    else { print(f"FAIL test_mutual_tail: {failures}") ; sys_exit(1) }
}
