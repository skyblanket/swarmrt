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

/* Run tests in a single .sw file. Returns number of failures. */
int sw_test_run_file(const char *path);

/* Run tests in all matching files in a directory.
 * Matches *_test.sw and test_*.sw patterns. Returns total failures. */
int sw_test_run_dir(const char *dir_path);

#endif /* SWARMRT_TEST_H */
