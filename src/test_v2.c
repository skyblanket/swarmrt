/*
 * SwarmRT v2 - Test & Benchmark
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include "swarmrt_v2.h"

static volatile int counter = 0;
static volatile int completed = 0;

static void inc_worker(void *arg) {
    (void)arg;
    __sync_fetch_and_add(&counter, 1);
    __sync_fetch_and_add(&completed, 1);
}

int main() {
    printf("\n=== SwarmRT v2 Test ===\n\n");
    
    int swarm = swarm_v2_init("test", 4);
    sleep(1);
    
    printf("Spawning 1000 processes...\n");
    for (int i = 0; i < 1000; i++) {
        sw_v2_spawn(inc_worker, NULL);
    }
    
    sleep(2);
    printf("Completed: %d/1000\n", completed);

    swarm_v2_stats(swarm);
    swarm_v2_shutdown(swarm);

    /* Each of the 1000 processes runs inc_worker exactly once, so a
     * correct scheduler lands completed == 1000. Anything else (the old
     * runaway-reschedule bug printed tens of millions) is a failure. */
    if (completed != 1000) {
        fprintf(stderr, "FAIL: expected 1000 completions, got %d\n", completed);
        return 1;
    }
    printf("PASS: v2 prototype ran 1000/1000 processes to completion\n");
    return 0;
}
