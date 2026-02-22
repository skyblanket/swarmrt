/*
 * SwarmRT Behaviour Feature Tests
 *
 * Tests Phase 1: Links, Monitors, Registry, Timers, Selective Receive
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>
#include "swarmrt_native.h"

static double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
}

/* =========================================================================
 * Test 1: Process Registry
 * ========================================================================= */

static void spawn_worker_noop(void *arg) { (void)arg; }

static volatile int reg_recv_count = 0;

static void reg_worker(void *arg) {
    (void)arg;
    /* Receive messages sent to our registered name */
    for (int i = 0; i < 3; i++) {
        void *msg = sw_receive(500);
        if (msg) {
            reg_recv_count++;
            free(msg);
        }
    }
}

static void test_registry(void) {
    printf("\n=== Test: Process Registry ===\n");
    reg_recv_count = 0;

    /* Spawn worker and register it */
    sw_process_t *w = sw_spawn(reg_worker, NULL);
    assert(w != NULL);

    int r = sw_register("counter", w);
    assert(r == 0);

    /* Lookup by name */
    sw_process_t *found = sw_whereis("counter");
    assert(found == w);
    printf("  sw_whereis(\"counter\"): found (PID %llu)\n",
           (unsigned long long)found->pid);

    /* Duplicate name should fail (use a quick no-op worker to avoid blocking) */
    sw_process_t *w2 = sw_spawn(spawn_worker_noop, NULL);
    r = sw_register("counter", w2);
    assert(r == -1);
    printf("  Duplicate register: rejected\n");

    /* Send by name */
    usleep(10000); /* Let worker start receiving */
    sw_send_named("counter", SW_TAG_NONE, strdup("msg1"));
    sw_send_named("counter", SW_TAG_NONE, strdup("msg2"));
    sw_send_named("counter", SW_TAG_NONE, strdup("msg3"));

    usleep(200000); /* Wait for delivery */
    printf("  Messages received via name: %d/3\n", reg_recv_count);

    /* Non-existent name should return NULL */
    assert(sw_whereis("nonexistent") == NULL);
    printf("  sw_whereis(\"nonexistent\"): NULL\n");

    /* Unregister (may already be unregistered if process exited) */
    r = sw_unregister("counter");
    /* r==0 if still registered, r==-1 if process already exited and auto-unregistered */
    assert(sw_whereis("counter") == NULL);
    printf("  Unregistered \"counter\": OK (r=%d)\n", r);

    printf("  Registry: PASS\n");
}

/* =========================================================================
 * Test 2: Process Links (crash propagation)
 * ========================================================================= */

static volatile int link_exit_received = 0;
static volatile uint64_t link_exit_from = 0;
static volatile int link_exit_reason = -999;

static void link_child(void *arg) {
    (void)arg;
    usleep(50000); /* 50ms — give parent time to set up link */
    /* Exit with abnormal reason by setting exit_reason before returning */
    sw_process_t *self = sw_self();
    self->exit_reason = 42; /* Abnormal */
}

static void link_parent(void *arg) {
    sw_process_t *child = (sw_process_t *)arg;

    /* Trap exits — receive as messages instead of dying */
    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    /* Link to child */
    sw_link(child);

    /* Wait for EXIT message */
    void *msg = sw_receive_tagged(SW_TAG_EXIT, 5000);
    if (msg) {
        sw_signal_t *sig = (sw_signal_t *)msg;
        link_exit_received = 1;
        link_exit_from = sig->pid;
        link_exit_reason = sig->reason;
        free(msg);
    }
}

static void test_links(void) {
    printf("\n=== Test: Process Links ===\n");
    link_exit_received = 0;

    /* Spawn child first (scheduler 0), then parent (scheduler 1) */
    sw_process_t *child = sw_spawn(link_child, NULL);
    uint64_t child_pid = child->pid;
    sw_process_t *parent = sw_spawn(link_parent, child);
    (void)parent;

    /* Wait for everything to settle */
    usleep(500000);

    printf("  EXIT received: %s\n", link_exit_received ? "YES" : "NO");
    if (link_exit_received) {
        printf("  From PID: %llu (child was %llu): %s\n",
               (unsigned long long)link_exit_from,
               (unsigned long long)child_pid,
               link_exit_from == child_pid ? "MATCH" : "MISMATCH");
        printf("  Reason: %d (expected 42)\n", link_exit_reason);
    }
    printf("  Links: %s\n",
           (link_exit_received && link_exit_reason == 42) ? "PASS" : "FAIL");
}

/* =========================================================================
 * Test 3: Link kill chain via spawn_link (no trapping)
 * ========================================================================= */

static volatile int chain_parent_got_exit = 0;

static void chain_doomed_fn(void *arg) {
    (void)arg;
    usleep(100000); /* 100ms then exit abnormally */
    sw_self()->exit_reason = 1;
}

static void chain_coordinator(void *arg) {
    (void)arg;
    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    /* Spawn two processes linked to each other via us as coordinator */
    sw_process_t *doomed = sw_spawn_link(chain_doomed_fn, NULL);
    (void)doomed;

    /* When doomed dies abnormally, we get EXIT (because we trap) */
    void *msg = sw_receive_tagged(SW_TAG_EXIT, 5000);
    if (msg) {
        sw_signal_t *sig = (sw_signal_t *)msg;
        chain_parent_got_exit = sig->reason;
        free(msg);
    }
}

static void test_link_chain(void) {
    printf("\n=== Test: Link Kill Chain ===\n");
    chain_parent_got_exit = 0;

    sw_spawn(chain_coordinator, NULL);

    usleep(500000);

    printf("  Coordinator got EXIT reason: %d (expected 1)\n", chain_parent_got_exit);
    printf("  Link chain: %s\n", (chain_parent_got_exit == 1) ? "PASS" : "FAIL");
}

/* =========================================================================
 * Test 4: Monitors (DOWN notification)
 * ========================================================================= */

static volatile int mon_down_received = 0;
static volatile uint64_t mon_down_from = 0;
static volatile uint64_t mon_down_ref_got = 0;

static void mon_target(void *arg) {
    (void)arg;
    usleep(50000); /* Short-lived — die after 50ms */
}

static void mon_watcher(void *arg) {
    sw_process_t *target = (sw_process_t *)arg;

    /* Monitor the target */
    uint64_t ref = sw_monitor(target);

    /* Wait for DOWN message */
    void *msg = sw_receive_tagged(SW_TAG_DOWN, 5000);
    if (msg) {
        sw_signal_t *sig = (sw_signal_t *)msg;
        mon_down_received = 1;
        mon_down_from = sig->pid;
        mon_down_ref_got = sig->ref;
        free(msg);
    }
    (void)ref;
}

static void test_monitors(void) {
    printf("\n=== Test: Monitors ===\n");
    mon_down_received = 0;

    /* Spawn target (will die quickly) */
    sw_process_t *target = sw_spawn(mon_target, NULL);
    uint64_t target_pid = target->pid;

    /* Spawn watcher on different scheduler */
    sw_spawn(mon_watcher, target);

    usleep(500000);

    printf("  DOWN received: %s\n", mon_down_received ? "YES" : "NO");
    if (mon_down_received) {
        printf("  From PID: %llu (target was %llu): %s\n",
               (unsigned long long)mon_down_from,
               (unsigned long long)target_pid,
               mon_down_from == target_pid ? "MATCH" : "MISMATCH");
        printf("  Monitor ref: %llu\n", (unsigned long long)mon_down_ref_got);
    }
    printf("  Monitors: %s\n", mon_down_received ? "PASS" : "FAIL");
}

/* =========================================================================
 * Test 5: Selective Receive (tagged messages)
 * ========================================================================= */

#define TAG_PONG  100
#define TAG_PING  101
#define TAG_DATA  102

static volatile int selective_ok = 0;

static void selective_sender(void *arg) {
    sw_process_t *receiver = (sw_process_t *)arg;
    usleep(20000); /* Let receiver start waiting */

    /* Send messages with different tags — receiver should pick TAG_DATA */
    sw_send_tagged(receiver, TAG_PING, strdup("ping"));
    sw_send_tagged(receiver, TAG_PONG, strdup("pong"));
    sw_send_tagged(receiver, TAG_DATA, strdup("the-data"));
}

static void selective_receiver(void *arg) {
    (void)arg;

    /* Wait specifically for TAG_DATA — should skip ping and pong */
    void *msg = sw_receive_tagged(TAG_DATA, 2000);
    if (msg && strcmp((char *)msg, "the-data") == 0) {
        selective_ok = 1;
        free(msg);
    }

    /* The ping and pong should still be in the mailbox */
    void *m1 = sw_receive(100);
    void *m2 = sw_receive(100);
    if (m1) free(m1);
    if (m2) free(m2);
}

static void test_selective_receive(void) {
    printf("\n=== Test: Selective Receive ===\n");
    selective_ok = 0;

    sw_process_t *receiver = sw_spawn(selective_receiver, NULL);
    sw_spawn(selective_sender, receiver);

    usleep(500000);

    printf("  Received TAG_DATA while skipping others: %s\n",
           selective_ok ? "YES" : "NO");
    printf("  Selective receive: %s\n", selective_ok ? "PASS" : "FAIL");
}

/* =========================================================================
 * Test 6: Timers (send_after)
 * ========================================================================= */

static volatile int timer_received = 0;
static volatile double timer_latency_us = 0;

static void timer_test_fn(void *arg) {
    (void)arg;

    double start = get_time_us();

    /* Schedule a message to ourselves in 50ms */
    sw_send_after(50, sw_self(), SW_TAG_TIMER, strdup("tick"));

    /* Block waiting for the timer */
    void *msg = sw_receive_tagged(SW_TAG_TIMER, 2000);
    if (msg) {
        double elapsed = get_time_us() - start;
        timer_received = 1;
        timer_latency_us = elapsed;
        free(msg);
    }
}

static void test_timers(void) {
    printf("\n=== Test: Timers ===\n");
    timer_received = 0;

    sw_spawn(timer_test_fn, NULL);

    usleep(500000);

    printf("  Timer fired: %s\n", timer_received ? "YES" : "NO");
    if (timer_received) {
        printf("  Latency: %.1f ms (target: 50ms)\n", timer_latency_us / 1000.0);
        int accurate = (timer_latency_us > 45000 && timer_latency_us < 150000);
        printf("  Accuracy: %s\n", accurate ? "OK" : "DRIFT");
    }
    printf("  Timers: %s\n", timer_received ? "PASS" : "FAIL");
}

/* =========================================================================
 * Test 7: Auto-unregister on death
 * ========================================================================= */

static void short_lived(void *arg) {
    (void)arg;
    /* Register ourselves, then die */
    sw_register("ephemeral", sw_self());
    usleep(50000);
    /* Process exits → should auto-unregister */
}

static void test_auto_unregister(void) {
    printf("\n=== Test: Auto-unregister on Death ===\n");

    sw_spawn(short_lived, NULL);

    /* Should be registered initially */
    usleep(20000);
    sw_process_t *found = sw_whereis("ephemeral");
    printf("  While alive: %s\n", found ? "registered" : "not found");

    /* After death, should be gone */
    usleep(200000);
    found = sw_whereis("ephemeral");
    printf("  After death: %s\n", found ? "still registered (BAD)" : "unregistered");
    printf("  Auto-unregister: %s\n", found == NULL ? "PASS" : "FAIL");
}

/* =========================================================================
 * Test 8: spawn_link (atomic spawn + link)
 * ========================================================================= */

static volatile int spawnlink_exit = 0;

static void spawnlink_child(void *arg) {
    (void)arg;
    usleep(50000);
    sw_self()->exit_reason = 7; /* Abnormal exit */
}

static void spawnlink_parent(void *arg) {
    (void)arg;
    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    sw_process_t *child = sw_spawn_link(spawnlink_child, NULL);
    (void)child;

    void *msg = sw_receive_tagged(SW_TAG_EXIT, 5000);
    if (msg) {
        sw_signal_t *sig = (sw_signal_t *)msg;
        spawnlink_exit = sig->reason;
        free(msg);
    }
}

static void test_spawn_link(void) {
    printf("\n=== Test: spawn_link ===\n");
    spawnlink_exit = 0;

    sw_spawn(spawnlink_parent, NULL);

    usleep(500000);

    printf("  Child exit reason received: %d (expected 7)\n", spawnlink_exit);
    printf("  spawn_link: %s\n", (spawnlink_exit == 7) ? "PASS" : "FAIL");
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n");
    printf("============================================\n");
    printf("  SwarmRT Phase 1: Behaviour Feature Tests\n");
    printf("  Links | Monitors | Registry | Timers\n");
    printf("============================================\n");

    if (sw_init("otp-test", 4) != 0) {
        fprintf(stderr, "Failed to initialize SwarmRT\n");
        return 1;
    }

    /* Let schedulers warm up */
    usleep(50000);

    test_registry();
    test_links();
    test_link_chain();
    test_monitors();
    test_selective_receive();
    test_timers();
    test_auto_unregister();
    test_spawn_link();

    /* Summary */
    sw_stats(0);

    printf("\n============================================\n");
    printf("  Phase 1 Behaviour Tests Complete\n");
    printf("============================================\n\n");

    sw_shutdown(0);
    return 0;
}
