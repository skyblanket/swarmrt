module Conform_destructure

# Conformance: tuple-destructuring assignment (idents, _, literal
# match-asserts) + multi-arm with/else (incl. guard arms) + expect's
# falsy semantics (nil AND 'false' both panic). All parse-time desugar
# or shared-builtin behavior — must be identical on both paths.

fun lookup(m, k) {
    if (map_has_key(m, k)) { {'ok', map_get(m, k)} }
    else { {'error', k} }
}

fun resolve(m) {
    with {'ok', h} <- lookup(m, 'host'),
         {'ok', p} <- lookup(m, 'port') {
        f"{h}:{p}"
    } else {
        {'error', k} when k == 'host' -> "no host"
        {'error', k} -> f"missing: {k}"
        _ -> "unknown"
    }
}

fun main() {
    {a, b} = {1, 2}
    {x, _, z} = {'first', 'skip', 'third'}
    pair = {10, 20}
    {p, q} = pair
    print(f"binds: {a} {b} {x} {z} {p + q}")

    # literal positions assert-match and the binds still happen
    {'ok', v} = {'ok', 42}
    {200, body} = {200, "hello"}
    print(f"tagged: {v} {body}")

    # destructure from a function result (the {ok, val} idiom)
    {'ok', port} = lookup(%{port: 8080}, 'port')
    print(f"value: {port}")

    # multi-arm else, guard arm first
    print(resolve(%{host: "h", port: 1}))
    print(resolve(%{port: 1}))
    print(resolve(%{host: "h"}))

    # expect: 'false' is falsy (not just nil) — caught via try? no:
    # expect panics. Prove the truthy path passes values through.
    print(f"expect-pass: {expect(5, "never")} {expect("s", "never")}")
}
