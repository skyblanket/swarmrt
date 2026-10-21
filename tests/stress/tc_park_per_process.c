/*
 * Mutual tail-call park must be PER PROCESS, not per scheduler thread.
 *
 * Two processes on ONE scheduler thread each write the hop park, yield (so the
 * other process runs and writes its own park on the same thread), then re-read
 * the park through sw_tc_park(). With a __thread park the second writer
 * clobbers the first; with a per-process park each sees its own values.
 * Deterministic: no dependence on compiler TLS caching or thread migration.
 * (The .sw stress test tests/sw/test_tailcall_migrate.sw covers migration.)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <stdio.h>
#include <unistd.h>
#include <stdatomic.h>
#include "swarmrt_native.h"
#include "swarmrt_lang.h"

#define ROUNDS 1000
static atomic_int g_done, g_bad;

static void proc_entry(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int r = 0; r < ROUNDS; r++) {
        sw_tc_park_t *pk = sw_tc_park();
        pk->n = id;
        pk->args[0] = (sw_val_t *)(intptr_t)(id * 1000 + r);
        sw_yield();
        pk = sw_tc_park();
        if (pk->n != id || pk->args[0] != (sw_val_t *)(intptr_t)(id * 1000 + r))
            atomic_fetch_add(&g_bad, 1);
        sw_yield();
    }
    atomic_fetch_add(&g_done, 1);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (sw_init("tc-park", 1) != 0) { fprintf(stderr, "sw_init failed\n"); return 2; }
    sw_spawn(proc_entry, (void *)(intptr_t)1);
    sw_spawn(proc_entry, (void *)(intptr_t)2);
    for (int i = 0; i < 3000 && atomic_load(&g_done) < 2; i++) usleep(10000);
    sw_shutdown(0);
    if (atomic_load(&g_done) < 2 || atomic_load(&g_bad)) {
        printf("FAIL tc_park_per_process done=%d bad=%d\n",
               atomic_load(&g_done), atomic_load(&g_bad));
        return 1;
    }
    printf("OK tc_park_per_process\n");
    return 0;
}
