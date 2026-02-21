/*
 * SwarmRT v2 - Test & Benchmark
 */

#define _GNU_SOURCE
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
    
    return 0;
}
