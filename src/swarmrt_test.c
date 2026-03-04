/*
 * SwarmRT Test Framework
 *
 * Auto-discovers test_* functions in .sw files, runs each in an
 * isolated interpreter, and reports pass/fail with timing.
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include "swarmrt_test.h"
#include "swarmrt_lang.h"

/* ANSI colors for terminal output */
#define C_GREEN  "\033[32m"
#define C_RED    "\033[31m"
#define C_DIM    "\033[2m"
#define C_RESET  "\033[0m"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    size_t n = fread(buf, 1, len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static double time_ms(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 +
           (end->tv_nsec - start->tv_nsec) / 1e6;
}

int sw_test_run_file(const char *path) {
    char *source = read_file(path);
    if (!source) {
        fprintf(stderr, "test: cannot open '%s'\n", path);
        return 1;
    }

    void *ast = sw_lang_parse(source);
    free(source);
    if (!ast) {
        fprintf(stderr, "test: parse failed for '%s'\n", path);
        return 1;
    }

    /* Extract test_* function names from AST */
    node_t *mod = (node_t *)ast;
    const char *test_names[256];
    int ntest = 0;

    for (int i = 0; i < mod->v.mod.nfuns; i++) {
        const char *fname = mod->v.mod.funs[i]->v.fun.name;
        if (strncmp(fname, "test_", 5) == 0 && ntest < 256)
            test_names[ntest++] = fname;
    }

    if (ntest == 0) {
        printf(C_DIM "=== %s ===" C_RESET "\n", path);
        printf(C_DIM "  (no test_* functions found)" C_RESET "\n\n");
        return 0;
    }

    printf("=== %s ===\n", path);

    int passed = 0, failed = 0;
    struct timespec file_start, file_end;
    clock_gettime(CLOCK_MONOTONIC, &file_start);

    for (int t = 0; t < ntest; t++) {
        /* Create fresh interpreter for test isolation */
        sw_interp_t *interp = sw_lang_new(ast);
        interp->assert_failed = 0;
        interp->assert_msg[0] = '\0';
        interp->assert_line = 0;

        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        /* Run the test function */
        sw_val_t *result = sw_lang_call(interp, test_names[t], NULL, 0);
        (void)result;

        clock_gettime(CLOCK_MONOTONIC, &t_end);
        double elapsed = time_ms(&t_start, &t_end);

        if (interp->assert_failed) {
            failed++;
            printf(C_RED "  \u2717 %-30s" C_RESET C_DIM " (%.1fms)" C_RESET "\n",
                   test_names[t], elapsed);
            printf("    %s\n", interp->assert_msg);
            if (interp->assert_line > 0)
                printf("    at line %d\n", interp->assert_line);
        } else if (interp->error) {
            failed++;
            printf(C_RED "  \u2717 %-30s" C_RESET C_DIM " (%.1fms)" C_RESET "\n",
                   test_names[t], elapsed);
            printf("    error: %s\n", interp->error_msg);
        } else {
            passed++;
            printf(C_GREEN "  \u2713 %-30s" C_RESET C_DIM " (%.1fms)" C_RESET "\n",
                   test_names[t], elapsed);
        }

        /* Don't free interp->module_ast since it's shared across tests */
        interp->module_ast = NULL;
        sw_lang_free(interp);
    }

    clock_gettime(CLOCK_MONOTONIC, &file_end);
    double total = time_ms(&file_start, &file_end);

    printf("\n  %d test%s, " C_GREEN "%d passed" C_RESET,
           ntest, ntest == 1 ? "" : "s", passed);
    if (failed > 0)
        printf(", " C_RED "%d failed" C_RESET, failed);
    printf(C_DIM " (%.1fms)" C_RESET "\n\n", total);

    /* Free the shared AST now that all tests are done */
    /* node_free is internal to swarmrt_lang.c — we rely on process exit cleanup.
     * The AST was parsed once and we set module_ast=NULL before sw_lang_free
     * to avoid double-free. In practice this leaks ~few KB per test file which
     * is acceptable for a test runner. */

    return failed;
}

/* Check if filename matches test file patterns */
static int is_test_file(const char *name) {
    size_t len = strlen(name);
    if (len < 4) return 0;
    /* Must end with .sw */
    if (strcmp(name + len - 3, ".sw") != 0) return 0;
    /* Match *_test.sw or test_*.sw */
    if (len >= 8 && strcmp(name + len - 8, "_test.sw") == 0) return 1;
    if (strncmp(name, "test_", 5) == 0) return 1;
    return 0;
}

int sw_test_run_dir(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) {
        fprintf(stderr, "test: cannot open directory '%s'\n", dir_path);
        return 1;
    }

    int total_failures = 0;
    int files_run = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (!is_test_file(ent->d_name)) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
        total_failures += sw_test_run_file(path);
        files_run++;
    }
    closedir(d);

    if (files_run == 0) {
        printf("test: no test files found in '%s'\n", dir_path);
        printf("  (looking for *_test.sw or test_*.sw)\n");
    }

    return total_failures;
}
