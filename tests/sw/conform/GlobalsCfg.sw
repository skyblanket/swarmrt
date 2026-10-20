module GlobalsCfg

# Imported by t22_imported_globals.sw. Its top-level `let`s must be
# initialised on both paths: `swc run` used to leave them nil.

let C1 = 1
let C19 = 38
let SCALE = C19 * 2
let TABLE = [1, 2, 3]

fun c19() { C19 }
fun scaled() { SCALE + C1 }
fun table_len() { length(TABLE) }
