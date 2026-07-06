module Conform_stdin_pending

# Conformance: stdin_pending_push / stdin_take_pending must behave IDENTICALLY
# interpreter-vs-compiled. It's an in-memory byte ring (no terminal involved),
# so the whole sequence is deterministic on both paths — the compiled ring
# lives in swarmrt_builtins_studio.h and the interp ring in swarmrt_lang.c, and
# this program is the byte-for-byte proof that they agree.

fun main() {
    a = stdin_pending_push("abc")
    print(f"push1={a} take1={stdin_take_pending()} empty={stdin_take_pending()}")
    # multiple pushes accumulate and drain as one string, oldest-first.
    p2 = stdin_pending_push("foo")
    p3 = stdin_pending_push("bar")
    print(f"accum={stdin_take_pending()} p2={p2} p3={p3}")
    # a non-string arg is rejected (false) and pushes nothing.
    bad = stdin_pending_push(42)
    print(f"badarg={bad} after={stdin_take_pending()}")
}
