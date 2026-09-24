module Test_json_strict

# json_decode used to accept truncated input — `{"command":"rm -rf build`
# decoded to a complete map, so an LLM tool call cut off mid-argument still
# ran. Malformed input is nil now (trailing commas stay accepted), and
# json_encode always emits valid JSON: invalid UTF-8 → �, escaped map
# keys, NaN → null.

fun check(name, cond) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name) ; 1 }
}

fun is_nil(v) { if (v == nil) { 'true' } else { 'false' } }

fun main() {
    f = 0
    f = f + check("truncated_string", is_nil(json_decode("{\"command\":\"echo hi")))
    f = f + check("truncated_object", is_nil(json_decode("{\"command\":\"echo hi\"")))
    f = f + check("truncated_array", is_nil(json_decode("[1, 2")))
    f = f + check("trailing_data", is_nil(json_decode("{\"a\":1}{\"b\":2}")))
    f = f + check("missing_separator", is_nil(json_decode("{\"a\":1 \"b\":2}")))
    f = f + check("bad_token", is_nil(json_decode("{'a': 1}")))
    f = f + check("trailing_comma_ok", if (map_get(json_decode("{\"a\": [1, 2,],}"), 'a') == [1, 2]) { 'true' } else { 'false' })
    f = f + check("whitespace_ok", if (json_decode("  [1]\n") == [1]) { 'true' } else { 'false' })
    f = f + check("key_escaped", if (json_encode(json_decode("{\"a\\\"b\":1}")) == "{\"a\\\"b\":1}") { 'true' } else { 'false' })
    p = "/tmp/sw_json_strict_" ++ to_string(random_int(1, 1000000000)) ++ ".txt"
    file_write_bytes(p, bytes_from_ints([120, 255, 121]))
    raw = file_read(p)
    file_delete(p)
    f = f + check("invalid_utf8_replaced", if (json_encode(raw) == "\"x\\ufffdy\"") { 'true' } else { 'false' })
    if (f == 0) { print("OK json_strict 10/10") ; sys_exit(0) } else { print("FAIL json_strict") ; sys_exit(1) }
}
