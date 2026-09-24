module Conform_list_sharing

# Conformance: lists stay immutable values while append / cons / tl share
# backing storage under the hood (arena lists extend in place only when the
# extended list owns its store's edge). Every branch below must see exactly
# its own elements, on both paths.

fun build(n, acc) { if (n == 0) { acc } else { build(n - 1, [n | acc]) } }

fun grow(i, n, acc) { if (i == n) { acc } else { grow(i + 1, n, list_append(acc, i)) } }

fun sum(lst, acc) { if (length(lst) == 0) { acc } else { sum(tl(lst), acc + hd(lst)) } }

fun main() {
    a = [1, 2]
    b = list_append(a, 3)
    c = list_append(a, 4)
    print([a, b, c])
    d = [0 | a]
    e = [9 | a]
    print([a, d, e])
    t = tl(b)
    f = list_append(t, 5)
    g = list_append(b, 6)
    print([b, t, f, g])
    h = [7 | t]
    print([t, h, b])
    big = build(20000, [])
    print([length(big), hd(big), sum(big, 0)])
    up = grow(0, 20000, [])
    print([length(up), hd(up), sum(up, 0)])
    x = list_append(up, 'end')
    y = list_append(up, 'other')
    print([length(up), length(x), length(y)])
}
