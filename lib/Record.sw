# Record.sw — named, checked-shape data over tagged maps.
#
# Auto-discoverable: `import Record` works from anywhere (swc falls back to
# <swarmrt-root>/lib/ after the local directory).
#
# A "record" is just a map with a '__record' tag key. There is NO new language
# construct here — field access (`r.id`) already desugars to map_get, and
# `%{...}` patterns already match records. This module only adds (1) named,
# presence-checked construction and (2) a tag predicate, so agents write
# `.id / .from / .state` instead of fragile elem(t, 0/1/2).
#
# Two construction paths mirror the language's error model:
#   - Record.new   -> {'ok', rec} | {'error', reason}   (recoverable)
#   - Record.build -> rec, or panic on a missing field  (fail-fast)
#
# Everything here rides on builtins present in BOTH backends (map_get/map_put/
# map_has_key/map_remove/map_keys/panic/length/hd/tl/to_string/++), so it
# behaves identically under `swc build` and `swc run`. (Notably it does NOT use
# `is_map` — that builtin is codegen-only and absent from the interpreter, so
# depending on it would silently break `swc run`. `string_join` is a Std fn,
# not a builtin, so we inline a local `_join` to keep this module
# dependency-free.)

module Record

export [
    new, build, is, type_of, get, update, fields, to_map
]

# new(type, required, vals) — checked construction.
# `required` is a list of field-name atoms/strings; `vals` is a %{} of values.
# Returns {'ok', rec} when every required field is present, else {'error', msg}.
fun new(type, required, vals) {
    missing = _missing(required, vals, [])
    if (length(missing) == 0) {
        {'ok', map_put(vals, '__record', type)}
    } else {
        {'error', to_string(type) ++ ": missing field(s): " ++ _join(missing, ", ")}
    }
}

# build(type, required, vals) — fail-fast construction.
# Same as new/3 but panics on a missing field and returns the record directly.
# Use when a missing field is a bug, not a runtime condition.
fun build(type, required, vals) {
    missing = _missing(required, vals, [])
    if (length(missing) == 0) {
        map_put(vals, '__record', type)
    } else {
        panic("Record.build " ++ to_string(type) ++
              ": missing field(s): " ++ _join(missing, ", "))
    }
}

# is(rec, type) — 'true' if rec is a record of the given type.
# map_get on a non-map returns nil in both backends, and nil never equals a
# type atom, so this needs no is_map guard.
fun is(rec, type) {
    if (map_get(rec, '__record') == type) { 'true' } else { 'false' }
}

# type_of(rec) — the record's type tag, or nil if untagged.
fun type_of(rec) { map_get(rec, '__record') }

# get(rec, field) — value of a field (same as rec.field / map_get).
# Provided for parity with update/3 and for when the field name is dynamic.
fun get(rec, field) { map_get(rec, field) }

# update(rec, field, val) — functional update of an EXISTING field.
# Panics if `field` is not already present, so a typo can't silently add a
# bogus key (the failure mode named fields are meant to prevent).
fun update(rec, field, val) {
    if (map_has_key(rec, field) == 'true') { map_put(rec, field, val) }
    else { panic("Record.update: no such field: " ++ to_string(field)) }
}

# fields(rec) — list of field-name keys, excluding the '__record' tag.
fun fields(rec) { _drop_tag(map_keys(rec), []) }

# to_map(rec) — the record as a plain map WITHOUT the '__record' tag
# (e.g. for JSON output that shouldn't leak the tag).
fun to_map(rec) { map_remove(rec, '__record') }

# ---- internals ----

# _missing(required, vals, acc) — required field names absent from vals.
fun _missing(required, vals, acc) {
    if (length(required) == 0) { reverse(acc) }
    else {
        k = hd(required)
        rest = tl(required)
        if (map_has_key(vals, k) == 'true') { _missing(rest, vals, acc) }
        else { _missing(rest, vals, [to_string(k) | acc]) }
    }
}

fun _drop_tag(keys, acc) {
    if (length(keys) == 0) { reverse(acc) }
    else {
        k = hd(keys)
        if (k == '__record') { _drop_tag(tl(keys), acc) }
        else { _drop_tag(tl(keys), [k | acc]) }
    }
}

# _join(lst, sep) — local string-join (string_join is a Std fn, NOT a builtin,
# so we can't call it unqualified from another module).
fun _join(lst, sep) {
    if (length(lst) == 0) { "" }
    else { if (length(lst) == 1) { hd(lst) }
    else { hd(lst) ++ sep ++ _join(tl(lst), sep) } }
}

fun reverse(lst) { _rev(lst, []) }
fun _rev(lst, acc) {
    if (length(lst) == 0) { acc }
    else { _rev(tl(lst), [hd(lst) | acc]) }
}
