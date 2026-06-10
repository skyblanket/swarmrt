module Conform_errors

# Conformance: error-handling semantics must be IDENTICAL in the
# interpreter (`swc run`) and a compiled binary (`swc build`).
# Covers: error() inside try, error() outside try (silent nil),
# try value passthrough, nested try, catch binding the message.
# NOTE: builtin-panic behavior (hd([]) etc.) is gated separately in
# conform/t02 because a panic ends the program — keep it last there.

fun safe_div(a, b) {
    try {
        if (b == 0) { error("divide by zero") }
        a / b
    } catch e {
        f"caught: {e}"
    }
}

fun main() {
    print(safe_div(10, 2))
    print(safe_div(10, 0))

    # error() OUTSIDE any try: documented to be silent; expression -> nil
    x = error("nobody hears this")
    print(f"after stray error: x={x}")

    # nested try: inner catches, outer sees normal value
    r = try {
        inner = try { error("inner") ; "unreached" } catch e { f"inner-caught:{e}" }
        inner ++ "|outer-ok"
    } catch e {
        "outer-caught"
    }
    print(r)

    # catch arm value is the expression result
    v = try { error("boom") ; 1 } catch e { 42 }
    print(f"v={v}")

    # dynamic extent: an error() raised in a CALLEE lands in the caller's
    # catch, and the callee's remaining statements do NOT run
    print(try { noisy_callee() } catch e { f"from-callee: {e}" })
}

fun noisy_callee() {
    error("raised in callee")
    print("callee continued — must not appear")
    "callee-normal-return"
}
