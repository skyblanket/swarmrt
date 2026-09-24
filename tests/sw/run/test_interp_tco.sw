module Test_interp_tco

# `swc run` / `swc test` / the REPL run on the tree-walking interpreter,
# which used to recurse on the C stack for every call and died after a few
# hundred iterations of ANY loop (recursion is sw's only loop). Tail calls
# are now run in place, including mutual tail recursion and the canonical
# receive-loop server. Exits 0 on success.

fun count(n, acc) { if (n == 0) { acc } else { count(n - 1, acc + 1) } }

fun ping(n) { if (n == 0) { 'done' } else { pong(n - 1) } }
fun pong(n) { if (n == 0) { 'done' } else { ping(n - 1) } }

fun server(n) {
    receive {
        {'inc', from} -> send(from, n + 1) ; server(n + 1)
        'stop' -> n
        after 2000 -> n
    }
}

fun drive(pid, i, k) {
    if (i == k) { 'ok' }
    else { send(pid, {'inc', self()}) ; receive { _ -> 0 after 2000 -> 0 } ; drive(pid, i + 1, k) }
}

fun classify(xs, evens, odds) {
    case xs {
        [] -> {evens, odds}
        [h | t] -> if (h % 2 == 0) { classify(t, evens + 1, odds) } else { classify(t, evens, odds + 1) }
    }
}

fun main() {
    fails = 0
    if (count(300000, 0) != 300000) { print("FAIL count") ; fails = fails + 1 }
    if (ping(100001) != 'done') { print("FAIL mutual") ; fails = fails + 1 }
    lp = fun(n, acc) { if (n == 0) { acc } else { lp(n - 1, acc + n) } }
    if (lp(50000, 0) != 1250025000) { print("FAIL lambda_loop") ; fails = fails + 1 }
    if (classify(1..20000, 0, 0) != {10000, 10000}) { print("FAIL case_arm_tail") ; fails = fails + 1 }
    pid = spawn(server(0))
    drive(pid, 0, 5000)
    send(pid, 'stop')
    if (fails == 0) { print("INTERP_TCO_OK") ; sys_exit(0) } else { sys_exit(1) }
}
