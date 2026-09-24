module Cf_undefined_names

# A name bound nowhere is a compile error on BOTH paths (it used to become
# an atom when compiled and nil when interpreted, so typos ran "fine").
# expect-error: undefined variable 'usr_name' — did you mean 'user_name'?
# expect-error: undefined variable 'totl' — did you mean 'total'?
# expect-error: undefined variable 'ok' (atoms are written quoted: 'ok')
# expect-error: 'to_string' is a builtin and can't be passed as a value yet

fun greet(user_name) { print("hi " ++ usr_name) }

fun main() {
    total = 5
    xs = map(fun(x) { x + totl }, [1, 2])
    status = ok
    ys = map(to_string, xs)
    greet("a")
}
