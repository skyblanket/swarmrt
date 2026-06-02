module Test_multimod_export_import

# Regression: an ENTRY module that declares BOTH `export [main]` AND
# `import X` had its function BODIES silently dropped in multi-module
# codegen. Root cause was in the parser (par_module): module-header
# clauses were parsed in a fixed order — all imports first, then at most
# one export — so an `export [...]` placed BEFORE an `import` left the
# import unrecorded AND made the function/global loop start at the
# unconsumed `import` token and bail immediately, dropping the whole body.
# The generated C then called an undefined `<Mod>_main` and cc failed.
#
# The header ordering here (export BEFORE import) is deliberate: it is the
# exact case that used to fail. Imports/exports must now parse in any order.

export [main]

import MmExportImport

fun assert_eq(name, actual, expected) {
    if (actual == expected) { print("PASS " ++ name) ; 0 }
    else {
        print("FAIL " ++ name ++ ": expected " ++ to_string(expected) ++
              ", got " ++ to_string(actual))
        1
    }
}

# Calls into the imported module must resolve, and this entry module's
# own function bodies (main + the local helper below) must be emitted.
fun local_double(x) { x * 2 }

fun main() {
    fails = 0
    fails = fails + assert_eq("imported_factorial", MmExportImport.factorial(5), 120)
    fails = fails + assert_eq("imported_add", MmExportImport.add(40, 2), 42)
    fails = fails + assert_eq("entry_local_fn", local_double(21), 42)
    if (fails == 0) { print("OK multimod_export_import 3/3") ; sys_exit(0) }
    else { print("FAIL multimod_export_import " ++ to_string(fails) ++ " failures") ; sys_exit(1) }
}
