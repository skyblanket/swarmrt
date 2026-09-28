module Test_map_store

# Maps built one key at a time used to be quadratic: every map_put copied
# the whole map and scanned it for the key (20,000 keys took 3.6 s). Maps
# now share a growable store with a hash index, and a put that adds a key
# extends the store in place when the map owns its edge (sw_val_map_put).
# These checks pin down that values stay immutable while sharing a store.

fun check(name, cond, got) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name ++ ": " ++ to_string(got)) ; 1 }
}

fun build(m, i, n) {
    if (i == n) { m } else { build(map_put(m, "k" ++ to_string(i), i), i + 1, n) }
}

fun build_int(m, i, n) {
    if (i == n) { m } else { build_int(map_put(m, i, i * 2), i + 1, n) }
}

fun all_present(m, i, n) {
    if (i == n) { 'true' }
    else { if (map_get(m, "k" ++ to_string(i)) == i) { all_present(m, i + 1, n) } else { 'false' } }
}

fun main() {
    f = 0

    base = build(map_new(), 0, 40)
    a = map_put(base, "left", 1)
    b = map_put(base, "right", 2)      # base no longer owns the edge: b gets a copy
    f = f + check("branch_a_sees_its_key", map_get(a, "left") == 1, a)
    f = f + check("branch_a_not_b_key", map_get(a, "right") == nil, map_get(a, "right"))
    f = f + check("branch_b_sees_its_key", map_get(b, "right") == 2, b)
    f = f + check("branch_b_not_a_key", map_get(b, "left") == nil, map_get(b, "left"))
    f = f + check("base_unchanged", map_size(base) == 40 && map_get(base, "left") == nil, map_size(base))

    r = map_put(a, "k3", 300)
    f = f + check("replace_new_value", map_get(r, "k3") == 300, map_get(r, "k3"))
    f = f + check("replace_leaves_old_map", map_get(a, "k3") == 3, map_get(a, "k3"))
    f = f + check("replace_keeps_size", map_size(r) == map_size(a), map_size(r))

    s = map_put(build(map_new(), 0, 30), 'k5', "atom")
    f = f + check("atom_and_string_are_distinct_keys", map_size(s) == 31, map_size(s))
    f = f + check("exact_key_wins", map_get(s, 'k5') == "atom" && map_get(s, "k5") == 5, map_get(s, 'k5'))
    f = f + check("atom_finds_string_key", map_get(s, 'k6') == 6, map_get(s, 'k6'))

    ints = build_int(map_new(), 0, 1000)
    f = f + check("int_keys", map_get(ints, 777) == 1554 && map_get(ints, "777") == nil, map_get(ints, 777))

    t = map_put(map_put(build(map_new(), 0, 30), {'t', 1}, "tuple"), "after", 9)
    f = f + check("tuple_key_after_index", map_get(t, {'t', 1}) == "tuple" && map_get(t, "after") == 9 && map_get(t, "k29") == 29, t)

    t0 = timestamp()
    big = build(map_new(), 0, 20000)
    ms = timestamp() - t0
    f = f + check("20k_keys_all_present", all_present(big, 0, 20000), map_size(big))
    # Timing is informational here (a slow host must not fail the gate):
    # the quadratic build took 3.6 s, the store builds 20k keys in ~25 ms.
    print(f"INFO 20k keys built in {ms}ms")

    if (f == 0) { print("OK map_store 14/14") ; sys_exit(0) } else { print("FAIL map_store") ; sys_exit(1) }
}
