module Test_std

# Verifies the Std module loads from <swarmrt-root>/lib/ AND that
# the canonical list/map/string helpers work as documented. This
# test also exercises the import resolver's stdlib fallback path —
# if it fails to even compile, the resolver itself is broken.

import Std

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun test_range() { assert_eq("range_0_5", Std.range(0, 5), [0, 1, 2, 3, 4]) }
fun test_take() { assert_eq("take_3", Std.take([1, 2, 3, 4, 5], 3), [1, 2, 3]) }
fun test_drop() { assert_eq("drop_2", Std.drop([1, 2, 3, 4, 5], 2), [3, 4, 5]) }
fun test_reverse() { assert_eq("reverse", Std.reverse([1, 2, 3]), [3, 2, 1]) }
fun test_sum() { assert_eq("sum", Std.sum([1, 2, 3, 4]), 10) }
fun test_product() { assert_eq("product", Std.product([1, 2, 3, 4]), 24) }
fun test_unique() { assert_eq("unique", Std.unique([1, 2, 2, 3, 1, 4]), [1, 2, 3, 4]) }
fun test_contains_yes() { assert_eq("contains_yes", Std.contains([1, 2, 3], 2), 'true') }
fun test_contains_no() { assert_eq("contains_no", Std.contains([1, 2, 3], 5), 'false') }
fun test_any_true() { assert_eq("any_true", Std.any([1, 2, 3], fun(x) { x > 2 }), 'true') }
fun test_all_true() { assert_eq("all_true", Std.all([2, 4, 6], fun(x) { x > 1 }), 'true') }
fun test_all_false() { assert_eq("all_false", Std.all([2, 4, 1], fun(x) { x > 1 }), 'false') }
fun test_count() { assert_eq("count", Std.count([1, 2, 3, 4, 5], fun(x) { x > 2 }), 3) }
fun test_last() { assert_eq("last", Std.last([1, 2, 3]), 3) }
fun test_zip() {
    p = Std.zip([1, 2, 3], ["a", "b", "c"])
    assert_eq("zip_len", length(p), 3)
}
fun test_partition() {
    r = Std.partition([1, 2, 3, 4], fun(x) { x > 2 })
    assert_eq("partition_yes", elem(r, 0), [3, 4])
}
fun test_sort() { assert_eq("sort", Std.sort([3, 1, 4, 1, 5, 9, 2, 6]), [1, 1, 2, 3, 4, 5, 6, 9]) }
fun test_flatten() { assert_eq("flatten", Std.flatten([[1, 2], [3], [4, 5]]), [1, 2, 3, 4, 5]) }
fun test_chunk() {
    c = Std.chunk_every([1, 2, 3, 4, 5], 2)
    assert_eq("chunk_len", length(c), 3)
}
fun test_intersperse() { assert_eq("intersperse", Std.intersperse([1, 2, 3], 0), [1, 0, 2, 0, 3]) }
fun test_max_by() { assert_eq("max_by", Std.max_by([1, 5, 3, 2], fun(x) { x }), 5) }
fun test_min_by() { assert_eq("min_by", Std.min_by([5, 1, 3, 2], fun(x) { x }), 1) }

# String helpers
fun test_string_join() { assert_eq("string_join", Std.string_join(["a", "b", "c"], ","), "a,b,c") }
fun test_string_repeat() { assert_eq("string_repeat", Std.string_repeat("ab", 3), "ababab") }
fun test_string_pad_left() { assert_eq("pad_left", Std.string_pad_left("7", 3, "0"), "007") }
fun test_string_pad_right() { assert_eq("pad_right", Std.string_pad_right("hi", 5, "-"), "hi---") }

# Map helpers
fun test_map_filter() {
    m = %{a: 1, b: 2, c: 3, d: 4}
    kept = Std.map_filter(m, fun(_k, v) { if (v > 2) { 'true' } else { 'false' } })
    assert_eq("map_filter_size", map_size(kept), 2)
}

fun main() {
    fails = 0
    fails = fails + test_range()
    fails = fails + test_take()
    fails = fails + test_drop()
    fails = fails + test_reverse()
    fails = fails + test_sum()
    fails = fails + test_product()
    fails = fails + test_unique()
    fails = fails + test_contains_yes()
    fails = fails + test_contains_no()
    fails = fails + test_any_true()
    fails = fails + test_all_true()
    fails = fails + test_all_false()
    fails = fails + test_count()
    fails = fails + test_last()
    fails = fails + test_zip()
    fails = fails + test_partition()
    fails = fails + test_sort()
    fails = fails + test_flatten()
    fails = fails + test_chunk()
    fails = fails + test_intersperse()
    fails = fails + test_max_by()
    fails = fails + test_min_by()
    fails = fails + test_string_join()
    fails = fails + test_string_repeat()
    fails = fails + test_string_pad_left()
    fails = fails + test_string_pad_right()
    fails = fails + test_map_filter()
    if (fails == 0) { print("OK std 27/27") ; sys_exit(0) }
    else { print("FAIL std " ++ to_string(fails)) ; sys_exit(1) }
}
