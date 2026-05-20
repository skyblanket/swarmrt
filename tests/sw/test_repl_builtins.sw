# test_repl_builtins.sw — exercises builtins that previously only worked
# in compiled code. Now runnable from both paths:
#
#   ./bin/swc build tests/sw/test_repl_builtins.sw -o /tmp/x && /tmp/x
#   ./bin/swc test tests/sw/test_repl_builtins.sw   # via interpreter
#
# When run through `swc test`, the test_* functions execute via the
# tree-walking interpreter — failures there mean the REPL surface has
# drifted from the codegen surface again.

module Test_repl_builtins

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# === Strings =================================================
fun test_string_sub() {
    fails = 0
    fails = fails + assert_eq("sub_basic", string_sub("hello", 0, 3), "hel")
    fails = fails + assert_eq("sub_mid", string_sub("hello", 1, 3), "ell")
    fails = fails + assert_eq("sub_clamp", string_sub("hi", 0, 10), "hi")
    fails = fails + assert_eq("sub_past_end", string_sub("hi", 5, 1), "")
    fails
}

fun test_string_replace() {
    fails = 0
    fails = fails + assert_eq("rep_basic", string_replace("foo bar foo", "foo", "baz"), "baz bar baz")
    fails = fails + assert_eq("rep_grow", string_replace("aaa", "a", "bb"), "bbbbbb")
    fails = fails + assert_eq("rep_none", string_replace("nochange", "x", "y"), "nochange")
    fails
}

fun test_string_truncate() {
    fails = 0
    fails = fails + assert_eq("trunc_short", string_truncate("short", 100), "short")
    fails = fails + assert_eq("trunc_long", string_truncate("hello world", 8), "hello wo")
    fails = fails + assert_eq("trunc_exact", string_truncate("hi", 5), "hi")
    fails
}

# === System ==================================================
fun test_sleep() {
    assert_eq("sleep_ok", sleep(1), 'ok')
}

fun test_random_int() {
    r = random_int(0, 100)
    if (r >= 0) {
        if (r <= 100) { print("PASS random_int_in_range") ; 0 }
        else { print("FAIL random_int_in_range: too high") ; 1 }
    } else { print("FAIL random_int_in_range: negative") ; 1 }
}

fun test_getenv() {
    p = getenv("PATH")
    if (p == nil) { print("FAIL getenv_path") ; 1 }
    else { print("PASS getenv_path") ; 0 }
}

# === Files ===================================================
fun test_file_roundtrip() {
    path = "/tmp/sw_test_repl_builtins.txt"
    _w = file_write(path, "hello from repl test")
    content = file_read(path)
    _d = file_delete(path)
    assert_eq("file_roundtrip", content, "hello from repl test")
}

fun test_file_exists() {
    fails = 0
    path = "/tmp/sw_test_repl_builtins_exists.txt"
    _w = file_write(path, "x")
    fails = fails + assert_eq("exists_true", file_exists(path), 'true')
    _d = file_delete(path)
    fails = fails + assert_eq("exists_false", file_exists(path), 'false')
    fails
}

fun test_file_list() {
    entries = file_list("/tmp")
    if (length(entries) > 0) { print("PASS file_list_nonempty") ; 0 }
    else { print("FAIL file_list_nonempty") ; 1 }
}

fun test_file_mkdir() {
    _m = file_mkdir("/tmp/sw_test_repl_dir")
    assert_eq("mkdir", file_exists("/tmp/sw_test_repl_dir"), 'true')
}

# === JSON ====================================================
fun test_json_escape() {
    fails = 0
    # json_escape wraps in quotes and escapes — produces a JSON string literal.
    fails = fails + assert_eq("esc_quotes", json_escape("a \"b\" c"), "\"a \\\"b\\\" c\"")
    fails = fails + assert_eq("esc_newline", json_escape("line\nbreak"), "\"line\\nbreak\"")
    fails
}

fun test_json_get() {
    fails = 0
    s = "{\"name\":\"akash\",\"age\":30}"
    fails = fails + assert_eq("json_get_str", json_get(s, "name"), "akash")
    fails = fails + assert_eq("json_get_int", json_get(s, "age"), 30)
    fails
}

# === Map ops =================================================
fun test_map_merge() {
    fails = 0
    a = %{x: 1, y: 2}
    b = %{y: 99, z: 3}
    m = map_merge(a, b)
    fails = fails + assert_eq("merge_x", map_get(m, 'x'), 1)
    fails = fails + assert_eq("merge_override", map_get(m, 'y'), 99)
    fails = fails + assert_eq("merge_new", map_get(m, 'z'), 3)
    fails = fails + assert_eq("merge_size", map_size(m), 3)
    fails
}

fun test_map_remove() {
    fails = 0
    m = %{a: 1, b: 2, c: 3}
    m2 = map_remove(m, 'b')
    fails = fails + assert_eq("remove_size", map_size(m2), 2)
    fails = fails + assert_eq("remove_gone", map_has_key(m2, 'b'), 'false')
    fails = fails + assert_eq("remove_kept", map_get(m2, 'a'), 1)
    fails
}

# === Shell ===================================================
fun test_shell() {
    fails = 0
    r = shell("echo hello")
    fails = fails + assert_eq("shell_exit", elem(r, 0), 0)
    if (string_contains(elem(r, 1), "hello")) { print("PASS shell_output") ; }
    else { print("FAIL shell_output") ; fails = fails + 1 }
    fails
}

# === SQLite ==================================================
fun test_db() {
    fails = 0
    db = db_open(":memory:")
    if (db >= 0) { print("PASS db_open") ; }
    else { print("FAIL db_open") ; fails = fails + 1 }
    _c = db_exec(db, "CREATE TABLE t(id INTEGER, name TEXT)")
    _i = db_exec(db, "INSERT INTO t VALUES(1, 'alice'), (2, 'bob')")
    rows = db_query(db, "SELECT id, name FROM t ORDER BY id", [])
    fails = fails + assert_eq("db_rows", length(rows), 2)
    first = hd(rows)
    fails = fails + assert_eq("db_first_id", map_get(first, "id"), 1)
    fails = fails + assert_eq("db_first_name", map_get(first, "name"), "alice")
    _close = db_close(db)
    fails
}

fun main() {
    fails = 0
    fails = fails + test_string_sub()
    fails = fails + test_string_replace()
    fails = fails + test_string_truncate()
    fails = fails + test_sleep()
    fails = fails + test_random_int()
    fails = fails + test_getenv()
    fails = fails + test_file_roundtrip()
    fails = fails + test_file_exists()
    fails = fails + test_file_list()
    fails = fails + test_file_mkdir()
    fails = fails + test_json_escape()
    fails = fails + test_json_get()
    fails = fails + test_map_merge()
    fails = fails + test_map_remove()
    fails = fails + test_shell()
    fails = fails + test_db()
    total = 27
    passed = total - fails
    if (fails == 0) { print("OK repl_builtins " ++ to_string(total) ++ "/" ++ to_string(total)) ; sys_exit(0) }
    else { print("FAIL repl_builtins " ++ to_string(passed) ++ "/" ++ to_string(total)) ; sys_exit(1) }
}
