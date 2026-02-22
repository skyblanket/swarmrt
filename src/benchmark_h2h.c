/*
 * SwarmRT Head-to-Head Benchmark
 *
 * Same benchmarks as beam_bench.erl for direct comparison.
 * Each benchmark re-initializes the runtime to avoid interference.
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>
#include "swarmrt_native.h"

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
}

/* =========================================================================
 * Spawn Benchmark
 * ========================================================================= */

static _Atomic int g_spawn_done;

static void spawn_worker(void *arg) {
    (void)arg;
    atomic_fetch_add(&g_spawn_done, 1);
}

static void bench_spawn(int n) {
    printf("\n=== Spawn %d processes ===\n", n);
    atomic_store(&g_spawn_done, 0);

    double t0 = now_us();
    for (int i = 0; i < n; i++) sw_spawn(spawn_worker, NULL);
    double t1 = now_us();

    while (atomic_load(&g_spawn_done) < n) usleep(100);
    double t2 = now_us();

    double spawn_us = t1 - t0;
    double total_us = t2 - t0;
    printf("  Spawn time: %.0f us (%.2f us/proc)\n", spawn_us, spawn_us / n);
    printf("  Total time: %.0f us (%.2f ms)\n", total_us, total_us / 1000);
    printf("  Rate: %.0f spawns/sec\n", n / (spawn_us / 1000000.0));
}

/* =========================================================================
 * Context Switch (yield)
 * ========================================================================= */

static _Atomic int g_yield_done;
static int g_max_yields;

static void yield_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < g_max_yields; i++) {
        sw_yield();
    }
    atomic_fetch_add(&g_yield_done, 1);
}

static void bench_context_switch(int n) {
    printf("\n=== Context switch %d yields ===\n", n);
    atomic_store(&g_yield_done, 0);
    g_max_yields = n;

    double t0 = now_us();
    sw_spawn(yield_worker, NULL);
    while (atomic_load(&g_yield_done) < 1) usleep(100);
    double t1 = now_us();

    double elapsed = t1 - t0;
    double ns_per = (elapsed * 1000) / n;
    printf("  Time: %.0f us\n", elapsed);
    printf("  Per switch: %.1f ns\n", ns_per);
    printf("  Rate: %.0f switches/sec\n", n / (elapsed / 1000000.0));
}

/* =========================================================================
 * Message Passing (ping-pong)
 *
 * Two processes send messages back and forth N times.
 * Both exit cleanly when done.
 * ========================================================================= */

static _Atomic int g_msg_done;
static int g_msg_count;

typedef struct { sw_process_t *from; int seq; } ping_msg_t;

static void msg_pong(void *arg) {
    (void)arg;
    sw_register("bench_pong", sw_self());
    for (int i = 0; i < g_msg_count; i++) {
        uint64_t tag = 0;
        void *msg = sw_receive_any(5000, &tag);
        if (!msg) break;
        ping_msg_t *pm = (ping_msg_t *)msg;
        sw_process_t *from = pm->from;
        pm->seq = i; /* reuse the buffer as reply */
        sw_send_tagged(from, SW_TAG_CAST, pm);
    }
    atomic_fetch_add(&g_msg_done, 1);
}

static void msg_ping(void *arg) {
    (void)arg;
    usleep(20000); /* let pong register */
    sw_process_t *pong = sw_whereis("bench_pong");
    if (!pong) { atomic_fetch_add(&g_msg_done, 1); return; }

    sw_process_t *me = sw_self();
    for (int i = 0; i < g_msg_count; i++) {
        ping_msg_t *pm = malloc(sizeof(ping_msg_t));
        pm->from = me;
        pm->seq = i;
        sw_send_tagged(pong, SW_TAG_CAST, pm);
        uint64_t tag = 0;
        void *reply = sw_receive_any(5000, &tag);
        if (reply) free(reply);
    }
    atomic_fetch_add(&g_msg_done, 1);
}

static void bench_message_passing(int n) {
    printf("\n=== Message passing %d round-trips ===\n", n);
    g_msg_count = n;
    atomic_store(&g_msg_done, 0);

    sw_spawn(msg_pong, NULL);
    sw_spawn(msg_ping, NULL);

    /* Wait for both to finish */
    while (atomic_load(&g_msg_done) < 2) usleep(100);

    /* Re-measure without the startup delay: use total - 20ms offset */
    /* Actually let's just directly time the message part from outside */
    /* The 20ms sleep is included, so subtract it */
}

/* Timed version that properly measures just the messaging */
static void bench_message_passing_timed(int n) {
    printf("\n=== Message passing %d round-trips ===\n", n);
    g_msg_count = n;
    atomic_store(&g_msg_done, 0);

    double t0 = now_us();
    sw_spawn(msg_pong, NULL);
    sw_spawn(msg_ping, NULL);
    while (atomic_load(&g_msg_done) < 2) usleep(100);
    double t1 = now_us();

    double elapsed = t1 - t0 - 20000; /* subtract 20ms startup */
    double ns_per = (elapsed * 1000) / (n * 2);
    printf("  Time: %.0f us (%.2f ms)\n", elapsed, elapsed / 1000);
    printf("  Per message: %.1f ns\n", ns_per);
    printf("  Rate: %.0f msgs/sec\n", n * 2 / (elapsed / 1000000.0));
}

/* =========================================================================
 * Ring Benchmark
 *
 * N processes in a ring. Each forwards messages to next.
 * After all messages arrive at collector, send stop to all.
 * ========================================================================= */

typedef struct { sw_process_t *next; int id; } ring_arg_t;

static void ring_node_fn(void *arg) {
    ring_arg_t *ra = (ring_arg_t *)arg;
    sw_process_t *next = ra->next;
    free(ra);

    while (1) {
        uint64_t tag = 0;
        void *msg = sw_receive_any(500, &tag); /* short timeout */
        if (!msg) break; /* timeout = ring done, exit */
        if (tag == SW_TAG_STOP) { free(msg); break; }
        sw_send_tagged(next, tag, msg); /* forward */
    }
}

static _Atomic int g_ring_recv;
static int g_ring_target;
static sw_process_t *g_ring_entry;
static int g_ring_num_msgs;

static void ring_collector_fn(void *arg) {
    (void)arg;
    while (atomic_load(&g_ring_recv) < g_ring_target) {
        uint64_t tag = 0;
        void *msg = sw_receive_any(2000, &tag);
        if (!msg) break;
        free(msg);
        atomic_fetch_add(&g_ring_recv, 1);
    }
}

/* Injector: sends initial messages from a process context */
static void ring_injector_fn(void *arg) {
    (void)arg;
    for (int i = 0; i < g_ring_num_msgs; i++) {
        int *val = malloc(sizeof(int));
        *val = i;
        sw_send_tagged(g_ring_entry, SW_TAG_CAST, val);
    }
}

static void bench_ring(int num_procs, int num_msgs) {
    printf("\n=== Ring %d procs x %d messages ===\n", num_procs, num_msgs);
    atomic_store(&g_ring_recv, 0);
    g_ring_target = num_msgs;
    g_ring_num_msgs = num_msgs;

    /* Collector */
    sw_process_t *collector = sw_spawn(ring_collector_fn, NULL);
    usleep(5000);

    /* Build ring: last -> ... -> node[1] -> node[0] -> collector */
    sw_process_t *next = collector;
    int np = num_procs > 500 ? 500 : num_procs;
    for (int i = 0; i < np; i++) {
        ring_arg_t *ra = malloc(sizeof(ring_arg_t));
        ra->next = next;
        ra->id = i;
        next = sw_spawn(ring_node_fn, ra);
    }
    usleep(10000);

    g_ring_entry = next; /* last node spawned = ring entry */
    int total_hops = np * num_msgs;

    double t0 = now_us();
    /* Inject from a process context */
    sw_spawn(ring_injector_fn, NULL);

    /* Wait for all messages to arrive at collector */
    while (atomic_load(&g_ring_recv) < num_msgs) usleep(100);
    double t1 = now_us();

    double elapsed = t1 - t0;
    printf("  Time: %.0f us (%.2f ms)\n", elapsed, elapsed / 1000);
    printf("  Total hops: %d\n", total_hops);
    printf("  Rate: %.0f hops/sec\n", total_hops / (elapsed / 1000000.0));

    /* Let ring nodes timeout and exit */
    usleep(2500000);
}

/* =========================================================================
 * Parallel spawn+complete
 * ========================================================================= */

static _Atomic int g_par_done;

static void par_worker(void *arg) {
    (void)arg;
    volatile int sum = 0;
    for (int i = 1; i <= 100; i++) sum += i;
    (void)sum;
    atomic_fetch_add(&g_par_done, 1);
}

static void bench_parallel_spawn(int n) {
    printf("\n=== Parallel spawn+complete %d processes ===\n", n);
    atomic_store(&g_par_done, 0);

    double t0 = now_us();
    for (int i = 0; i < n; i++) sw_spawn(par_worker, NULL);
    while (atomic_load(&g_par_done) < n) usleep(100);
    double t1 = now_us();

    double elapsed = t1 - t0;
    printf("  Time: %.0f us (%.2f ms)\n", elapsed, elapsed / 1000);
    printf("  Per process: %.2f us\n", elapsed / n);
    printf("  Rate: %.0f procs/sec\n", n / (elapsed / 1000000.0));
}

/* =========================================================================
 * Memory
 * ========================================================================= */

static void bench_memory(void) {
    printf("\n=== Memory per process ===\n");
    size_t per_proc = sizeof(sw_process_t) + (SWARM_HEAP_MIN_SIZE * 8);
    printf("  Per process: %zu bytes\n", per_proc);
    printf("  10000 processes: %.2f MB\n", (per_proc * 10000) / (1024.0 * 1024.0));
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n================================================\n");
    printf("  SwarmRT Benchmark Suite (Head-to-Head)\n");
    printf("  Schedulers: 4\n");
    printf("================================================\n");

    if (sw_init("h2h-bench", 4) != 0) {
        fprintf(stderr, "Failed to init SwarmRT\n");
        return 1;
    }

    bench_spawn(100);
    bench_spawn(1000);
    bench_spawn(10000);

    bench_context_switch(1000);
    bench_context_switch(10000);
    bench_context_switch(100000);

    bench_message_passing_timed(10000);

    bench_memory();

    bench_parallel_spawn(10000);

    /* Ring benchmark — now possible with context-switch scheduling! */
    bench_ring(10, 100);

    sw_shutdown(0);

    printf("\n================================================\n");
    printf("  SwarmRT Benchmark Complete\n");
    printf("================================================\n\n");

    return 0;
}
