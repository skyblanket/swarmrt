module Cf_forward_global_ref

# A global's initialiser runs at load, in declaration order. Reading a later
# global used to segfault the compiled program and print nil when interpreted.
# expect-error: global 'A' is used before it is defined

let B = A + 1
let A = 41

fun main() {
    print(B)
}
