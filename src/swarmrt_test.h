/*
 * SwarmRT Test Framework
 *
 * Convention: functions named test_* are auto-discovered and run.
 * Uses assert/assert_eq/assert_ne builtins for assertions.
 *
 * Usage: swc test [file.sw ...] [dir/]
 *
 * otonomy.ai
 */

#ifndef SWARMRT_TEST_H
#define SWARMRT_TEST_H

/* Run tests in a single .sw file. Its imports resolve as in `swc run`
 * (siblings, installed packages, then `lib_dir`). Returns number of failures. */
int sw_test_run_file(const char *path, const char *lib_dir);

/* Run tests in all matching files in a directory.
 * Matches *_test.sw and test_*.sw patterns. Returns total failures. */
int sw_test_run_dir(const char *dir_path, const char *lib_dir);

#endif /* SWARMRT_TEST_H */
