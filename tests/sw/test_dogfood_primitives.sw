module Test_dogfood_primitives

# Regression coverage for the dogfood-audit primitives added in the
# iterate-dogfood-1 pass: binary-safe file I/O, math builtins, char codes,
# direct byte construction, map_get default, and parameterized db_exec.
# Compiled-path tests (compile + run); the interpreter twins live in
# tests/sw/repl/test_dogfood_primitives_interp.sw.

fun check_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# --- binary-safe file I/O: bytes with NULs round-trip byte-identical ---
fun test_file_bytes_roundtrip() {
    b = bytes_from_ints([72, 0, 73, 0, 255])
    file_write_bytes("/tmp/sw_dogfood_bytes.bin", b)
    r = file_read_bytes("/tmp/sw_dogfood_bytes.bin")
    fails = 0
    fails = fails + check_eq("read_bytes_size", byte_size(r), 5)
    fails = fails + check_eq("read_bytes_nul", byte_at(r, 1), 0)      # embedded NUL survives
    fails = fails + check_eq("read_bytes_high", byte_at(r, 4), 255)
    fails = fails + check_eq("write_bytes_ok", file_write_bytes("/tmp/sw_dogfood_bytes.bin", b), 'ok')
    file_delete("/tmp/sw_dogfood_bytes.bin")
    fails
}

# --- direct byte construction ---
fun test_bytes_from_ints() {
    b = bytes_from_ints([0, 1, 256, 257])   # masked to 0..255
    fails = 0
    fails = fails + check_eq("bfi_size", byte_size(b), 4)
    fails = fails + check_eq("bfi_mask256", byte_at(b, 2), 0)    # 256 & 0xFF
    fails = fails + check_eq("bfi_mask257", byte_at(b, 3), 1)    # 257 & 0xFF
    fails
}

fun test_byte() {
    one = byte(321)   # 321 & 0xFF == 65
    fails = 0
    fails = fails + check_eq("byte_size1", byte_size(one), 1)
    fails = fails + check_eq("byte_val", byte_at(one, 0), 65)
    fails
}

# --- math ---
fun test_math() {
    fails = 0
    fails = fails + check_eq("sqrt", math_sqrt(16), 4.0)
    fails = fails + check_eq("sqrt_int_arg", math_sqrt(144), 12.0)
    fails = fails + check_eq("pow", math_pow(2, 10), 1024.0)
    fails = fails + check_eq("floor", math_floor(3.7), 3)
    fails = fails + check_eq("ceil", math_ceil(3.2), 4)
    fails = fails + check_eq("round", math_round(3.5), 4)
    fails = fails + check_eq("exp0", math_exp(0), 1.0)
    fails = fails + check_eq("log1", math_log(1), 0.0)
    fails = fails + check_eq("to_float", to_float(7), 7.0)
    fails
}

# --- char codes ---
fun test_ord() {
    fails = 0
    fails = fails + check_eq("ord_A", ord("A"), 65)
    fails = fails + check_eq("ord_z", ord("z"), 122)
    fails = fails + check_eq("ord_empty", ord(""), -1)
    fails = fails + check_eq("cp_at", codepoint_at("ABC", 2), 67)
    fails = fails + check_eq("cp_at0", codepoint_at("ABC", 0), 65)
    fails = fails + check_eq("cp_oob", codepoint_at("ABC", 9), -1)
    fails
}

# --- map_get 3-arg default ---
fun test_map_get_default() {
    m = map_put(map_new(), "k", 42)
    fails = 0
    fails = fails + check_eq("mg_hit", map_get(m, "k", 99), 42)
    fails = fails + check_eq("mg_default", map_get(m, "missing", 99), 99)
    fails = fails + check_eq("mg_2arg", map_get(m, "missing"), nil)
    fails
}

# --- parameterized db_exec ---
fun test_db_exec_params() {
    h = db_open(":memory:")
    db_exec(h, "CREATE TABLE t (id INTEGER, name TEXT)")
    r1 = db_exec(h, "INSERT INTO t (id, name) VALUES (?, ?)", [1, "alice"])
    r2 = db_exec(h, "INSERT INTO t (id, name) VALUES (?, ?)", [2, "bob"])
    rows = db_query(h, "SELECT name FROM t WHERE id = ?", [2])
    fails = 0
    fails = fails + check_eq("dbx_ins1", r1, 'ok')
    fails = fails + check_eq("dbx_ins2", r2, 'ok')
    fails = fails + check_eq("dbx_count", length(rows), 1)
    fails = fails + check_eq("dbx_val", map_get(hd(rows), "name"), "bob")
    db_close(h)
    fails
}

# --- file_append builtin ---
fun test_file_append() {
    file_delete("/tmp/sw_dogfood_append.txt")
    file_append("/tmp/sw_dogfood_append.txt", "a\n")
    file_append("/tmp/sw_dogfood_append.txt", "b\n")
    data = file_read("/tmp/sw_dogfood_append.txt")
    file_delete("/tmp/sw_dogfood_append.txt")
    check_eq("file_append", data, "a\nb\n")
}

fun main() {
    fails = 0
    fails = fails + test_file_bytes_roundtrip()
    fails = fails + test_bytes_from_ints()
    fails = fails + test_byte()
    fails = fails + test_math()
    fails = fails + test_ord()
    fails = fails + test_map_get_default()
    fails = fails + test_db_exec_params()
    fails = fails + test_file_append()
    if (fails == 0) { print("OK dogfood_primitives all/all") ; sys_exit(0) }
    else { print("FAIL dogfood_primitives " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
