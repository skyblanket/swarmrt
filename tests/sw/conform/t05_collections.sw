module Conform_collections

# Conformance: lists (hd/tl/length/append/nth), multi-head cons
# patterns, maps (put/get/keys/has_key/merge/remove), nested access,
# structural equality, map/filter/reduce builtins.

# (patterns in function heads are not part of the language — dispatch
# through case instead; see KNOWN gotchas)
fun second(xs) {
    case xs {
        [_, b | _] -> b
        _ -> 'too_short'
    }
}

fun main() {
    xs = [1, 2, 3, 4, 5]
    print(f"hd={hd(xs)} tl={tl(xs)} len={length(xs)}")
    print(f"append={list_append(xs, 6)} second={second(xs)}")
    print(f"map={map(fun(x) { x * 2 }, xs)}")
    print(f"filter={filter(xs, fun(x) { x % 2 == 1 })}")
    print(f"reduce={reduce(fun(acc, x) { acc + x }, xs, 0)}")

    m = %{name: "sw", tags: ["fast", "small"], meta: %{v: 2}}
    m2 = map_put(m, 'extra', 'yes')
    print(f"get={map_get(m2, 'name')} nested={map_get(map_get(m2, 'meta'), 'v')}")
    print(f"has={map_has_key(m2, 'extra')} miss={map_get(m2, 'nope')}")
    m3 = map_remove(m2, 'extra')
    print(f"removed-has={map_has_key(m3, 'extra')} nkeys={length(map_keys(m3))}")
    merged = map_merge(%{a: 1, b: 2}, %{b: 20, c: 30})
    print(f"merged-b={map_get(merged, 'b')} merged-c={map_get(merged, 'c')}")

    # structural equality across types
    print(f"eq={[1, 2] == [1, 2]} {%{x: 1} == %{x: 1}} {{1, 'a'} == {1, 'a'}} {[1] == [2]}")
}
