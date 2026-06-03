# test_json_expect_interp.sw — Std.json_expect must work in the
# tree-walking interpreter (`swc run`), not just the compiled path. It is a
# pure decode-and-validate helper, so it should behave identically in both.
# This guards against builtin-parity drift (it must NOT reach for a builtin
# the interpreter lacks, like is_map — an earlier version did and aborted
# the run). Self-checking: prints JE_INTERP_OK and exits 0 only on full pass.

module Test_json_expect_interp
import Std

fun chk(name, actual, expected) {
    if (actual == expected) { 0 }
    else {
        print("JE_INTERP_FAIL " ++ name ++ ": expected " ++
              to_string(expected) ++ ", got " ++ to_string(actual))
        1
    }
}

# Tag extractor so we can assert on the {'ok'|'error', _} tag in the interp
# without relying on tuple == map comparisons.
fun tag(res) { elem(res, 0) }

fun main() {
    fails = 0
    ok = Std.json_expect("{\"a\":1,\"b\":2}", %{required: ['a', 'b']})
    fails = fails + chk("ok_tag", tag(ok), 'ok')
    fails = fails + chk("ok_val", map_get(elem(ok, 1), 'a'), 1)

    miss = Std.json_expect("{\"a\":1}", %{required: ['a', 'b']})
    fails = fails + chk("missing_tag", tag(miss), 'error')

    bad = Std.json_expect("not json", %{required: []})
    fails = fails + chk("bad_tag", tag(bad), 'error')

    arr = Std.json_expect("[1,2,3]", %{required: []})
    fails = fails + chk("array_tag", tag(arr), 'error')

    if (fails == 0) { print("JE_INTERP_OK") ; sys_exit(0) }
    else { sys_exit(1) }
}
