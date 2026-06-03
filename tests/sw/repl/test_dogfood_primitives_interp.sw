# test_dogfood_primitives_interp.sw — interpreter-side coverage of the
# dogfood-audit primitives (binary-safe file I/O, math, char codes, direct
# byte construction, map_get default, parameterized db_exec). The compiled
# twin is tests/sw/test_dogfood_primitives.sw. Run via:
#
#   ./bin/swc test tests/sw/repl/test_dogfood_primitives_interp.sw
#
# Guards against codegen/interpreter drift on the new builtins.

module Test_dogfood_primitives_interp

fun test_file_bytes_roundtrip() {
    b = bytes_from_ints([72, 0, 73, 0, 255])
    assert_eq(file_write_bytes("/tmp/sw_dogfood_bytes_i.bin", b), 'ok')
    r = file_read_bytes("/tmp/sw_dogfood_bytes_i.bin")
    assert_eq(byte_size(r), 5)
    assert_eq(byte_at(r, 1), 0)      # embedded NUL survives
    assert_eq(byte_at(r, 4), 255)
    file_delete("/tmp/sw_dogfood_bytes_i.bin")
}

fun test_bytes_from_ints() {
    b = bytes_from_ints([0, 1, 256, 257])
    assert_eq(byte_size(b), 4)
    assert_eq(byte_at(b, 2), 0)     # 256 & 0xFF
    assert_eq(byte_at(b, 3), 1)     # 257 & 0xFF
}

fun test_byte() {
    one = byte(321)                 # 321 & 0xFF == 65
    assert_eq(byte_size(one), 1)
    assert_eq(byte_at(one, 0), 65)
}

fun test_math() {
    assert_eq(math_sqrt(16), 4.0)
    assert_eq(math_sqrt(144), 12.0)
    assert_eq(math_pow(2, 10), 1024.0)
    assert_eq(math_floor(3.7), 3)
    assert_eq(math_ceil(3.2), 4)
    assert_eq(math_round(3.5), 4)
    assert_eq(math_exp(0), 1.0)
    assert_eq(math_log(1), 0.0)
    assert_eq(to_float(7), 7.0)
}

fun test_ord() {
    assert_eq(ord("A"), 65)
    assert_eq(ord("z"), 122)
    assert_eq(ord(""), -1)
    assert_eq(codepoint_at("ABC", 2), 67)
    assert_eq(codepoint_at("ABC", 0), 65)
    assert_eq(codepoint_at("ABC", 9), -1)
}

fun test_map_get_default() {
    m = map_put(map_new(), "k", 42)
    assert_eq(map_get(m, "k", 99), 42)
    assert_eq(map_get(m, "missing", 99), 99)
    assert_eq(map_get(m, "missing"), nil)
}

fun test_db_exec_params() {
    h = db_open(":memory:")
    db_exec(h, "CREATE TABLE t (id INTEGER, name TEXT)")
    assert_eq(db_exec(h, "INSERT INTO t (id, name) VALUES (?, ?)", [1, "alice"]), 'ok')
    assert_eq(db_exec(h, "INSERT INTO t (id, name) VALUES (?, ?)", [2, "bob"]), 'ok')
    rows = db_query(h, "SELECT name FROM t WHERE id = ?", [2])
    assert_eq(length(rows), 1)
    assert_eq(map_get(hd(rows), "name"), "bob")
    db_close(h)
}

fun test_file_append() {
    file_delete("/tmp/sw_dogfood_append_i.txt")
    file_append("/tmp/sw_dogfood_append_i.txt", "a\n")
    file_append("/tmp/sw_dogfood_append_i.txt", "b\n")
    data = file_read("/tmp/sw_dogfood_append_i.txt")
    file_delete("/tmp/sw_dogfood_append_i.txt")
    assert_eq(data, "a\nb\n")
}

# Functional primitives — interpreter parity (these are codegen-only
# prelude builtins elsewhere; the interpreter run path needs them too).
fun test_functional() {
    assert_eq(reduce(fun(acc, x) { acc + x }, [1, 2, 3, 4], 0), 10)
    assert_eq(map(fun(x) { x * 2 }, [1, 2, 3]), [2, 4, 6])
    assert_eq(filter(fun(x) { x > 2 }, [1, 2, 3, 4]), [3, 4])
}
