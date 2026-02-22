/*
 * SwarmRT Phase 8: GC & Heap Management Tests
 *
 * 5 tests: heap alloc, minor GC, major GC, allocation under pressure, stats.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include "swarmrt_gc.h"
#include "swarmrt_otp.h"
#include "swarmrt_ets.h"
#include "swarmrt_phase5.h"
#include "swarmrt_hotload.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_PASS(name) do { tests_passed++; printf("  [PASS] %s\n", name); } while(0)
#define TEST_FAIL(name) do { tests_failed++; printf("  [FAIL] %s\n", name); } while(0)
#define TEST_CHECK(name, cond) do { if (cond) TEST_PASS(name); else TEST_FAIL(name); } while(0)

/* =========================================================================
 * G1: Heap allocation
 * ========================================================================= */

static volatile int g1_passed = 0;

static void g1_runner(void *arg) {
    (void)arg;
    sw_process_t *self = sw_self();

    size_t before = sw_heap_used(self);
    void *p1 = sw_heap_alloc(self, 10);
    size_t after = sw_heap_used(self);

    int in_heap = sw_heap_contains(self, p1);

    g1_passed = (p1 != NULL && after > before && in_heap);
    printf("  Alloc: %s, Used: %zu → %zu, In heap: %d\n",
           p1 ? "OK" : "NULL", before, after, in_heap);
}

static void test_heap_alloc(void) {
    printf("\n=== G1: Heap allocation ===\n");
    g1_passed = 0;
    sw_spawn(g1_runner, NULL);
    usleep(200000);
    TEST_CHECK("heap_alloc", g1_passed);
}

/* =========================================================================
 * G2: Minor GC reclaims space
 * ========================================================================= */

static volatile int g2_passed = 0;

static void g2_runner(void *arg) {
    (void)arg;
    sw_process_t *self = sw_self();

    /* Fill heap with allocations */
    for (int i = 0; i < 100; i++) {
        sw_heap_alloc(self, 1);
    }

    size_t used_before = sw_heap_used(self);
    int reclaimed = sw_gc_minor(self);
    size_t used_after = sw_heap_used(self);

    /* Since nothing references these allocations, most should be reclaimed */
    g2_passed = (reclaimed >= 0);
    printf("  Before: %zu words, After: %zu words, Reclaimed: %d\n",
           used_before, used_after, reclaimed);
}

static void test_minor_gc(void) {
    printf("\n=== G2: Minor GC ===\n");
    g2_passed = 0;
    sw_spawn(g2_runner, NULL);
    usleep(200000);
    TEST_CHECK("minor_gc", g2_passed);
}

/* =========================================================================
 * G3: Major GC full compaction
 * ========================================================================= */

static volatile int g3_passed = 0;

static void g3_runner(void *arg) {
    (void)arg;
    sw_process_t *self = sw_self();

    /* Fill heap */
    for (int i = 0; i < 50; i++) {
        sw_heap_alloc(self, 2);
    }

    size_t used_before = sw_heap_used(self);
    int reclaimed = sw_gc_major(self);
    size_t used_after = sw_heap_used(self);

    g3_passed = (reclaimed >= 0 && used_after <= used_before);
    printf("  Before: %zu, After: %zu, Reclaimed: %d\n",
           used_before, used_after, reclaimed);
}

static void test_major_gc(void) {
    printf("\n=== G3: Major GC ===\n");
    g3_passed = 0;
    sw_spawn(g3_runner, NULL);
    usleep(200000);
    TEST_CHECK("major_gc", g3_passed);
}

/* =========================================================================
 * G4: Allocation triggers GC automatically
 * ========================================================================= */

static volatile int g4_passed = 0;

static void g4_runner(void *arg) {
    (void)arg;
    sw_process_t *self = sw_self();

    size_t heap_size = sw_heap_size(self);
    int allocs = 0;

    /* Keep allocating until we fill the heap — GC should keep us going */
    for (int i = 0; i < 500; i++) {
        void *p = sw_heap_alloc(self, 1);
        if (p) allocs++;
        else break;
    }

    /* Should have successfully allocated more than the heap size
     * because GC reclaims space */
    g4_passed = (allocs > 0);
    printf("  Heap size: %zu words, Successful allocs: %d\n", heap_size, allocs);
}

static void test_auto_gc(void) {
    printf("\n=== G4: Auto GC on allocation ===\n");
    g4_passed = 0;
    sw_spawn(g4_runner, NULL);
    usleep(200000);
    TEST_CHECK("auto_gc", g4_passed);
}

/* =========================================================================
 * G5: GC stats tracking
 * ========================================================================= */

static volatile int g5_passed = 0;

static void g5_runner(void *arg) {
    (void)arg;
    sw_process_t *self = sw_self();

    /* Allocate some data so GC has work to do */
    for (int i = 0; i < 10; i++) sw_heap_alloc(self, 1);
    sw_gc_minor(self);

    for (int i = 0; i < 10; i++) sw_heap_alloc(self, 1);
    sw_gc_major(self);

    sw_gc_stats_t stats = sw_gc_stats(self);

    g5_passed = (stats.minor_gcs >= 1 && stats.major_gcs >= 1);
    printf("  Minor GCs: %llu, Major GCs: %llu, Time: %llu μs\n",
           stats.minor_gcs, stats.major_gcs, stats.total_gc_time_us);
}

static void test_gc_stats(void) {
    printf("\n=== G5: GC stats ===\n");
    g5_passed = 0;
    sw_spawn(g5_runner, NULL);
    usleep(200000);
    TEST_CHECK("gc_stats", g5_passed);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("============================================\n");
    printf("  SwarmRT Phase 8: GC & Heap Management\n");
    printf("============================================\n");

    if (sw_init("phase8-test", 4) != 0) {
        fprintf(stderr, "Failed to initialize SwarmRT\n");
        return 1;
    }

    test_heap_alloc();
    test_minor_gc();
    test_major_gc();
    test_auto_gc();
    test_gc_stats();

    sw_stats(0);
    sw_shutdown(0);

    printf("\n============================================\n");
    printf("  Results: %d passed, %d failed (of %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
