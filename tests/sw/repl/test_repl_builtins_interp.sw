# test_repl_builtins_interp.sw — interpreter-only coverage of builtins
# that previously only worked in compiled mode. Run via:
#
#   ./bin/swc test tests/sw/repl/test_repl_builtins_interp.sw
#
# Each test_* function is invoked through the tree-walking interpreter
# (see swarmrt_test.c). Uses the 2-arg builtin `assert_eq(actual, expected)`
# directly rather than defining a user-level 3-arg wrapper (which would
# be shadowed by the builtin in interpreter mode anyway).
#
# Failures here mean the REPL/test surface has drifted from the codegen
# surface — re-run `tests/sw/test_repl_builtins.sw` to confirm whether
# the codegen path is also broken, then patch both interpreter and
# compiled versions to agree.

module Test_repl_builtins_interp

fun test_string_sub() {
    assert_eq(string_sub("hello", 0, 3), "hel")
    assert_eq(string_sub("hello", 1, 3), "ell")
    assert_eq(string_sub("hi", 0, 10), "hi")
    assert_eq(string_sub("hi", 5, 1), "")
}

fun test_string_replace() {
    assert_eq(string_replace("foo bar foo", "foo", "baz"), "baz bar baz")
    assert_eq(string_replace("aaa", "a", "bb"), "bbbbbb")
    assert_eq(string_replace("nochange", "x", "y"), "nochange")
}

fun test_string_truncate() {
    assert_eq(string_truncate("short", 100), "short")
    assert_eq(string_truncate("hello world", 8), "hello wo")
    assert_eq(string_truncate("hi", 5), "hi")
}

fun test_sleep_returns_ok() {
    assert_eq(sleep(1), 'ok')
}

fun test_random_int_in_range() {
    r = random_int(0, 10)
    assert(r >= 0)
    assert(r <= 10)
}

fun test_getenv_path() {
    p = getenv("PATH")
    assert(p != nil)
}

fun test_file_roundtrip() {
    path = "/tmp/sw_repl_interp_test.txt"
    _w = file_write(path, "hi")
    content = file_read(path)
    _d = file_delete(path)
    assert_eq(content, "hi")
}

fun test_file_exists() {
    path = "/tmp/sw_repl_interp_exists.txt"
    _w = file_write(path, "x")
    assert_eq(file_exists(path), 'true')
    _d = file_delete(path)
    assert_eq(file_exists(path), 'false')
}

fun test_file_list_nonempty() {
    entries = file_list("/tmp")
    assert(length(entries) > 0)
}

fun test_file_mkdir() {
    _m = file_mkdir("/tmp/sw_repl_interp_dir")
    assert_eq(file_exists("/tmp/sw_repl_interp_dir"), 'true')
}

fun test_json_escape() {
    assert_eq(json_escape("hi"), "\"hi\"")
    assert_eq(json_escape("a\nb"), "\"a\\nb\"")
}

fun test_json_get_simple() {
    s = "{\"name\":\"akash\",\"age\":30}"
    assert_eq(json_get(s, "name"), "akash")
    assert_eq(json_get(s, "age"), 30)
}

fun test_map_merge() {
    a = %{x: 1, y: 2}
    b = %{y: 99, z: 3}
    m = map_merge(a, b)
    assert_eq(map_get(m, 'x'), 1)
    assert_eq(map_get(m, 'y'), 99)
    assert_eq(map_get(m, 'z'), 3)
    assert_eq(map_size(m), 3)
}

fun test_map_remove() {
    m = %{a: 1, b: 2, c: 3}
    m2 = map_remove(m, 'b')
    assert_eq(map_size(m2), 2)
    assert_eq(map_has_key(m2, 'b'), 'false')
    assert_eq(map_get(m2, 'a'), 1)
}

fun test_shell_echo() {
    r = shell("echo hello")
    code = elem(r, 0)
    out = elem(r, 1)
    assert_eq(code, 0)
    assert(string_contains(out, "hello"))
}

fun test_db_roundtrip() {
    db = db_open(":memory:")
    assert(db >= 0)
    _c = db_exec(db, "CREATE TABLE t(id INTEGER, name TEXT)")
    _i = db_exec(db, "INSERT INTO t VALUES(1, 'alice'), (2, 'bob')")
    rows = db_query(db, "SELECT id, name FROM t ORDER BY id", [])
    assert_eq(length(rows), 2)
    first = hd(rows)
    assert_eq(map_get(first, "id"), 1)
    assert_eq(map_get(first, "name"), "alice")
    _close = db_close(db)
}
