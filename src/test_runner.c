/*
 * SwarmRT Test Runner
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "swarmrt.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do { \
    printf("  " #name "... "); \
    test_##name(); \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
    return; \
} while(0)

/* === Tests === */

TEST(init_shutdown) {
    swarm_init(4);
    swarm_stats();
    swarm_shutdown();
}

TEST(spawn_single) {
    swarm_init(2);
    
    volatile int ran = 0;
    void worker(void *arg) {
        *(volatile int *)arg = 1;
    }
    
    sw_spawn(worker, (void *)&ran);
    usleep(50000);
    
    if (!ran) FAIL("Worker did not run");
    
    swarm_shutdown();
}

TEST(spawn_multiple) {
    swarm_init(4);
    
    volatile int counter = 0;
    void worker(void *arg) {
        __sync_fetch_and_add((volatile int *)arg, 1);
    }
    
    for (int i = 0; i < 100; i++) {
        sw_spawn(worker, (void *)&counter);
    }
    
    usleep(200000);
    
    if (counter != 100) FAIL("Not all workers ran");
    
    swarm_shutdown();
}

TEST(message_passing) {
    swarm_init(2);
    
    /* This test needs full message passing implementation */
    /* For now just verify API exists */
    
    swarm_shutdown();
}

int main(void) {
    printf("\n=== SwarmRT Test Suite ===\n\n");
    
    RUN(init_shutdown);
    RUN(spawn_single);
    RUN(spawn_multiple);
    /* RUN(message_passing); */ /* Not fully implemented */
    
    printf("\n=== Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    printf("\n");
    
    return tests_failed > 0 ? 1 : 0;
}
