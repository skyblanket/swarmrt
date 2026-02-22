/*
 * SwarmRT-BEAM Simple Test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include "swarmrt_beam.h"

static volatile int counter = 0;

static void simple_worker(void *arg) {
    int id = (int)(uintptr_t)arg;
    printf("  [Worker] Process %d started\n", id);
    
    for (int i = 0; i < 100; i++) {
        __sync_fetch_and_add(&counter, 1);
    }
    
    printf("  [Worker] Process %d completed\n", id);
}

int main() {
    printf("\n=== SwarmRT-BEAM Simple Test ===\n\n");
    
    printf("Initializing swarm with 2 schedulers...\n");
    int swarm = swarm_beam_init("simple_test", 2);
    if (swarm < 0) {
        fprintf(stderr, "Failed to initialize swarm\n");
        return 1;
    }
    
    printf("Waiting for schedulers to start...\n");
    sleep(1);
    
    printf("Spawning 10 processes...\n");
    for (int i = 0; i < 10; i++) {
        sw_beam_process_t *p = sw_beam_spawn_on(swarm, i % 2, simple_worker, (void *)(uintptr_t)i);
        if (!p) {
            fprintf(stderr, "Failed to spawn process %d\n", i);
        }
    }
    
    printf("Waiting for processes to complete...\n");
    sleep(3);
    
    printf("\nCounter: %d (expected: 1000)\n", counter);
    
    if (counter == 1000) {
        printf("✓ Test PASSED\n");
    } else {
        printf("✗ Test FAILED\n");
    }
    
    swarm_beam_stats(swarm);
    swarm_beam_shutdown(swarm);
    
    printf("\n=== Test completed ===\n");
    return (counter == 1000) ? 0 : 1;
}
