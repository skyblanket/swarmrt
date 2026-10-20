module Cf_builtin_value_optional_arity

# map_get takes an optional 3rd argument (default), so it has no single
# arity to wrap as a value. It used to be wrapped at arity 2, which silently
# dropped the default: g(m, k, 0) returned nil instead of 0.
# expect-error: 'map_get' is a builtin and can't be passed as a value yet

fun main() {
    g = map_get
    print(g(%{a: 1}, "b", 0))
}
