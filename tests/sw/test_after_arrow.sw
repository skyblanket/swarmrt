module Test_after_arrow

# The Erlang-reflex `after MS -> body` arrow form must parse and fire,
# alongside the canonical `after MS { body }` block form. (Compiled
# suite: receive needs the real scheduler.)

fun arrow_waiter(from) {
    receive {
        'never' -> 'wrong'
        after 30 -> send(from, 'arrow_fired') ; 'ok'
    }
}

fun block_waiter(from) {
    receive {
        'never' -> 'wrong'
        after 30 { send(from, 'block_fired') ; 'ok' }
    }
}

fun main() {
    failures = 0
    spawn(arrow_waiter(self()))
    spawn(block_waiter(self()))

    a = receive { 'arrow_fired' -> 1 after 2000 { 0 } }
    b = receive { 'block_fired' -> 1 after 2000 { 0 } }

    failures = failures + (if (a == 1) { print("PASS after_arrow_fires") ; 0 }
                           else { print("FAIL after_arrow_fires") ; 1 })
    failures = failures + (if (b == 1) { print("PASS after_block_fires") ; 0 }
                           else { print("FAIL after_block_fires") ; 1 })

    if (failures == 0) { print("OK test_after_arrow 2/2") ; sys_exit(0) }
    else { print(f"FAIL test_after_arrow: {failures}") ; sys_exit(1) }
}
