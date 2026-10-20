module Conform_imported_globals

# An imported module's top-level `let` globals must read the same value
# interpreted (`swc run`) and compiled (`swc build`). Regression: the
# interpreter left them nil.

import GlobalsCfg

let LOCAL = 5
let LOCAL2 = LOCAL * 2

fun main() {
    print(GlobalsCfg.c19())
    print(GlobalsCfg.scaled())
    print(GlobalsCfg.table_len())
    print(LOCAL2)
}
