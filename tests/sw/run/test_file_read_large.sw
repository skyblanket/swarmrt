module Test_file_read_large

# #198: file_read returned nil (seen as "") for any file over 1 MiB on the
# compiled path. It must return the whole file, in both backends (compiled
# here + the tests/sw/run/ copy via swc run). An empty file is "", not nil.

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# A string of 2^k copies of s.
fun doubled(s, k) { if (k == 0) { s } else { doubled(s ++ s, k - 1) } }

fun roundtrip(name, path, s) {
    file_write(path, s)
    r = file_read(path)
    file_delete(path)
    fails = assert_eq(name ++ "_len", string_length(r), string_length(s))
    fails + assert_eq(name ++ "_same", r == s, true)
}

fun main() {
    path = "/tmp/sw_file_read_large_" ++ uuid() ++ ".txt"
    mib = doubled("abcdefgh", 17)                       # 1048576 bytes
    fails = 0
    fails = fails + roundtrip("exactly_1mib", path, mib)
    fails = fails + roundtrip("1mib_plus_1", path, mib ++ "z")
    fails = fails + roundtrip("3mib_plus", path, mib ++ mib ++ mib ++ "tail")
    fails = fails + roundtrip("empty", path, "")
    fails = fails + assert_eq("empty_is_string", typeof(file_read_empty(path)), "string")
    fails = fails + assert_eq("missing_is_nil", file_read(path ++ ".missing"), nil)
    if (fails == 0) { print("OK file_read_large") ; sys_exit(0) }
    else { print("FAIL file_read_large") ; sys_exit(1) }
}

fun file_read_empty(path) {
    file_write(path, "")
    r = file_read(path)
    file_delete(path)
    r
}
