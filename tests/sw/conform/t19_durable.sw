module Conform_durable

# checkpoint / restore / checkpoint_delete give the same answers interpreted
# and compiled — both paths share one implementation (swarmrt_node.c) and one
# encoding. run_conform.sh points SW_STATE_DB at a fresh per-program db.

import Durable

fun show(key, v) {
    ok = checkpoint(key, v)
    back = restore(key)
    print(key ++ " " ++ to_string(ok) ++ " " ++ typeof(back) ++ " " ++
          to_string(back) ++ " " ++ to_string(back == v))
}

fun step(s) {
    if (s < 5) { {'next', s + 1} } else { {'done', s * 10} }
}

fun main() {
    show("int", 42)
    show("neg", 0 - 7)
    show("float", 2.5)
    show("string", "héllo")
    show("atom", 'idle')
    show("list", [1, "two", 'three'])
    show("tuple", {'ok', 1})
    show("map", %{"n" => 1, 'k' => [2.5]})
    show("bytes", bytes_from_ints([0, 7, 255]))
    show("nested", {'agent', %{"hist" => [{1, 'a'}, {2, 'b'}]}, []})
    print(to_string(restore("tuple") == [ 'ok', 1 ]))
    print(to_string(restore("missing")))
    print(to_string(checkpoint("pid", self())))
    print(to_string(checkpoint("fun", fun(x) { x })))
    print(to_string(checkpoint('k', 1)))
    print(to_string(checkpoint_delete("int")) ++ " " ++ to_string(restore("int")))
    print(to_string(Durable.load("fresh", 'init')))
    print(to_string(Durable.loop("job", 0, fun(s) { step(s) })) ++ " " ++ to_string(restore("job")))
}
