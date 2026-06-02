module MmExportImport

# Helper module imported by test_multimod_export_import.sw.
# Named WITHOUT the `test_` prefix so the run_tests.sh `test_*.sw`
# glob does not try to compile it as a standalone test (it has no main()).

fun factorial(n) {
  if (n <= 1) { 1 } else { n * factorial(n - 1) }
}

fun add(a, b) { a + b }
