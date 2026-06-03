module MmGlobalLet

# Helper module imported by test_multimod_global_let.sw. Named WITHOUT the
# `test_` prefix so the run_tests.sh `test_*.sw` glob does not compile it
# standalone (it has no main()).
#
# Regression target: a top-level `let` in an IMPORTED (non-entry) module
# used to be left uninitialized at runtime — _main_entry only ran the
# ENTRY module's globals initializer, so MyMod.BASE stayed a NULL sw_val_t*
# and the first use (here, the `++` in greet/base) SIGSEGV'd.

let BASE = "v"
let COUNT = 7

fun base() { BASE }
fun count() { COUNT }
fun greet(name) { BASE ++ ":" ++ name }
