# test_interp_compiled_parity.sw — interpreter-only coverage of five
# interpreter↔compiled divergences in the tree-walker (swarmrt_lang.c).
# Run via:
#
#   ./bin/swc test tests/sw/repl/test_interp_compiled_parity.sw
#
# Each test runs through the tree-walking interpreter (swarmrt_test.c).
# The compiled path (swc build) is the reference of truth; these tests
# pin the interpreter to match it. If one fails, the interpreter has
# drifted back to a silent-wrong / lax result.

module Test_interp_compiled_parity

# === 1. && / || short-circuit =============================================
# The right operand must NOT be evaluated when the left already decides
# the result, so panic() never fires. Compiled: false && _ → :false.

fun test_and_short_circuits() {
    # If && evaluated panic("boom"), the test would error out, not pass.
    r = false && panic("boom")
    assert_eq(r, 'false')
}

fun test_or_short_circuits() {
    r = true || panic("boom")
    assert_eq(r, 'true')
}

fun test_and_or_logic_results() {
    assert_eq(true && true, 'true')
    assert_eq(true && false, 'false')
    assert_eq(false || true, 'true')
    assert_eq(false || false, 'false')
}

# === 2. ordering comparisons with float operands =========================
# < > <= >= used to return nil for any float operand.

fun test_float_ordering() {
    assert_eq(1.0 < 2.0, 'true')
    assert_eq(2.0 < 1.0, 'false')
    assert_eq(1 < 2.0, 'true')
    assert_eq(2.0 > 1, 'true')
    assert_eq(2.0 <= 2.0, 'true')
    assert_eq(3.0 >= 4.0, 'false')
}

# === 3. ++ polymorphic concat ============================================
# String concat coerces float/atom/bool/nil; list ++ list concatenates.

fun test_concat_coercion() {
    assert_eq("x" ++ 2.5, "x2.5")
    assert_eq("a" ++ true, "atrue")
    assert_eq("n" ++ nil, "nnil")
    assert_eq("at" ++ 'foo', "atfoo")
    assert_eq("i" ++ 7, "i7")
}

fun test_concat_lists() {
    assert_eq([1, 2] ++ [3, 4], [1, 2, 3, 4])
    assert_eq([] ++ [1], [1])
    assert_eq([1] ++ [], [1])
}

# === 4. json_encode → real JSON ==========================================

fun test_json_encode_map() {
    assert_eq(json_encode(%{name: "alice", age: 30}), "{\"name\":\"alice\",\"age\":30}")
}

fun test_json_encode_scalars() {
    assert_eq(json_encode([1, 2, 3]), "[1,2,3]")
    assert_eq(json_encode(nil), "null")
    assert_eq(json_encode(true), "true")
    assert_eq(json_encode(false), "false")
    assert_eq(json_encode("hi"), "\"hi\"")
    assert_eq(json_encode('atom'), "\"atom\"")
}

# === 5. strict panics matching compiled ==================================

fun test_div_by_zero_panics() {
    assert_raises(fun() { 10 / 0 }, "division by zero")
}

fun test_mod_by_zero_panics() {
    assert_raises(fun() { 10 % 0 }, "modulo by zero")
}

fun test_float_div_by_zero_panics() {
    assert_raises(fun() { 10.0 / 0.0 }, "division by zero")
}

fun test_elem_out_of_range_panics() {
    assert_raises(fun() { elem({1, 2}, 5) }, "out of range")
}

fun test_elem_negative_panics() {
    assert_raises(fun() { elem({1, 2}, 0 - 1) }, "out of range")
}

fun test_hd_empty_panics() {
    assert_raises(fun() { hd([]) }, "list is empty")
}

fun test_tl_empty_panics() {
    assert_raises(fun() { tl([]) }, "list is empty")
}

fun test_hd_nil_panics() {
    assert_raises(fun() { hd(nil) }, "not a list")
}

# === abs handles floats (shared bug, both paths) =========================

fun test_abs_float() {
    assert_eq(abs(-2.5), 2.5)
    assert_eq(abs(2.5), 2.5)
    assert_eq(abs(-3), 3)
}
