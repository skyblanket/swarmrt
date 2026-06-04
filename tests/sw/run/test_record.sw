module Test_record

# Record = named, checked-shape data over tagged maps (lib/Record.sw). This
# test runs in BOTH backends: compiled here, and via `swc run` in
# tests/sw/run/test_record.sw — the dual run is what proves the module uses
# only dual-backed builtins (no codegen-only is_map, no Std-only string_join).

import Record

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

fun assert_true(name, cond) {
    if (cond == 'true') { print("PASS " ++ name) ; 0 }
    else { print("FAIL " ++ name ++ ": condition false") ; 1 }
}

# Happy path: build a record, read fields via dot-access (which desugars to
# map_get) — named fields instead of positional tuple elements.
fun test_build_and_dot_access() {
    c = Record.build('Call', ['id', 'from', 'state'],
                     %{id: 7, from: "+1555", state: 'ringing'})
    fails = 0
    fails = fails + assert_eq("dot_id", c.id, 7)
    fails = fails + assert_eq("dot_from", c.from, "+1555")
    fails = fails + assert_eq("dot_state", c.state, 'ringing')
    fails
}

# new/3 returns {'ok', rec} on a complete shape.
fun test_new_ok() {
    r = Record.new('Call', ['id', 'state'], %{id: 1, state: 'idle'})
    case r {
        {'ok', rec} -> assert_eq("new_ok_state", rec.state, 'idle')
        {'error', msg} -> assert_true("new_ok_unexpected_error", 'false')
    }
}

# Failure case: a missing required field -> {'error', reason}, NOT a silent nil
# (the positional-tuple bug class records exist to prevent).
fun test_new_missing_field() {
    r = Record.new('Call', ['id', 'from', 'state'], %{id: 1, state: 'idle'})
    case r {
        {'ok', rec} -> assert_true("missing_should_fail", 'false')
        {'error', msg} -> assert_true("new_missing_is_error", 'true')
    }
}

# Tag predicate + %{} pattern dispatch on a record. Also: is/2 on a non-map
# must return 'false', not crash (proves the is_map-free guard is safe — the
# fix the adversarial review demanded for the interpreter path).
fun test_is_and_pattern_dispatch() {
    c = Record.build('Call', ['id', 'state'], %{id: 2, state: 'connected'})
    is_call = Record.is(c, 'Call')
    is_other = Record.is(c, 'Sms')
    dispatched = case c {
        %{__record: 'Call', state: s} -> s
        _ -> 'no_match'
    }
    fails = 0
    fails = fails + assert_eq("is_call", is_call, 'true')
    fails = fails + assert_eq("is_not_sms", is_other, 'false')
    fails = fails + assert_eq("is_on_nonmap", Record.is(42, 'Call'), 'false')
    fails = fails + assert_eq("pattern_dispatch", dispatched, 'connected')
    fails
}

# Functional update of an existing field; the original is unchanged (immutable).
fun test_update() {
    c = Record.build('Call', ['id', 'state'], %{id: 3, state: 'ringing'})
    c2 = Record.update(c, 'state', 'ended')
    fails = 0
    fails = fails + assert_eq("update_new", c2.state, 'ended')
    fails = fails + assert_eq("update_immutable_orig", c.state, 'ringing')
    fails
}

# to_map drops the tag (for JSON output that shouldn't leak '__record').
fun test_to_map_drops_tag() {
    c = Record.build('Call', ['id'], %{id: 9})
    plain = Record.to_map(c)
    assert_eq("to_map_no_tag", map_has_key(plain, '__record'), 'false')
}

# Records are just tagged maps, so they survive json_encode and a json_decode
# round-trip preserves the field values. (Atom tags/values become JSON strings
# on the way out — that's plain JSON, not a Record limitation — so we assert on
# values via map_get, which resolves consistently in both backends.)
fun test_json_round_trip() {
    r = Record.build('Job', ['id', 'name'], %{id: 42, name: "deploy"})
    back = json_decode(json_encode(r))
    fails = 0
    fails = fails + assert_eq("json_id_survives", map_get(back, "id"), 42)
    fails = fails + assert_eq("json_name_survives", map_get(back, "name"), "deploy")
    fails = fails + assert_eq("json_tag_survives", map_get(back, "__record"), "Job")
    fails
}

fun main() {
    fails = 0
    fails = fails + test_build_and_dot_access()
    fails = fails + test_new_ok()
    fails = fails + test_new_missing_field()
    fails = fails + test_is_and_pattern_dispatch()
    fails = fails + test_update()
    fails = fails + test_to_map_drops_tag()
    fails = fails + test_json_round_trip()
    if (fails == 0) { print("OK record 15/15") ; sys_exit(0) }
    else { print("FAIL record " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
