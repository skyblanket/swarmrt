module Case_tuple_body

# Seeds the fuzzer around the tuple-vs-block disambiguation added to par_case /
# par_receive (par_arm_brace_is_tuple). Mutating these bytes stresses the
# balanced {}/()/[]/%{} scan: a `{` after `->` whose top-level comma makes it a
# tuple result vs. a `{ stmt ; stmt }` block; nested braces, map literals,
# call-arg commas at depth, unterminated braces, EOF mid-scan, and the
# case-arm var-bind-inside-a-lambda path (lambda capture vs. local binder).

fun add3(a, b, c) { a + b + c }

fun classify(x) {
    case x {
        0 -> {'error', 'zero'}
        1 -> { y = 10 ; y + x }
        2 -> { add3(x, x, x) }
        3 -> { {'nested', x} }
        4 -> {'triple', x, x}
        5 -> {'wrapped', add3(1, 2, 3)}
        6 -> %{tag: 'six', val: x, items: [1, 2, 3]}
        m -> {'default', m}
    }
}

fun in_lambda() {
    f = fn(x) { case x { 0 -> {'z', 0} ; m -> {'got', m} } }
    g = fn(y) { case y { {'pair', a, b} -> {'sum', a + b} ; n -> {'one', n} } }
    f(7)
}

fun server(from) {
    receive {
        {'ask', who} -> {'reply', 42}
        {'pos', n} when n > 0 -> {'result', 'positive', n}
        'stop' -> {'done'}
    }
    # degenerate shapes the mutator chews on:
    #   -> {
    #   -> {'a',
    #   -> { {{{ }}} }
    #   -> %{a: 1, b: 2,
}

fun main() {
    classify(0)
    in_lambda()
}
