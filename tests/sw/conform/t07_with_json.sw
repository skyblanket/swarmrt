module Conform_with_json

# Conformance: with-expression (happy chain, first-failure routing to
# else) + JSON encode/decode round-trips + json_get + conversions.

fun step(tag, v) {
    if (v > 0) { {'ok', v * 2} } else { {'error', tag} }
}

fun pipeline(a, b) {
    with {'ok', x} <- step("a", a),
         {'ok', y} <- step("b", b) {
        f"sum={x + y}"
    } else {
        {'error', t} -> f"failed-at={t}"
    }
}

fun main() {
    print(pipeline(2, 5))
    print(pipeline(2, 0 - 1))
    print(pipeline(0 - 1, 5))

    obj = %{name: "sw", version: 2, tags: ["a", "b"], nested: %{ok: 'true'}}
    js = json_encode(obj)
    back = json_decode(js)
    print(f"roundtrip-name={map_get(back, 'name')} v={map_get(back, 'version')}")
    print(f"json_get={json_get(js, "nested.ok")}")

    print(f"to_int={to_int("42")} to_float={to_float("2.5")} bad={to_int("xyz")}")
    print(f"typeof={typeof(1)} {typeof(1.0)} {typeof("s")} {typeof('a')} {typeof([])} {typeof(%{})} {typeof({1, 2})}")
}
