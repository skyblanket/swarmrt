/*
 * SwarmRT Phase 3: Task (async/await) + ETS (concurrent tables)
 *
 * 7 Task tests + 8 ETS tests = 15 total.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdatomic.h>
#include "swarmrt_otp.h"
#include "swarmrt_task.h"
#include "swarmrt_ets.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_PASS(name) do { tests_passed++; printf("  [PASS] %s\n", name); } while(0)
#define TEST_FAIL(name) do { tests_failed++; printf("  [FAIL] %s\n", name); } while(0)
#define TEST_CHECK(name, cond) do { if (cond) TEST_PASS(name); else TEST_FAIL(name); } while(0)

/* =========================================================================
 * T1: Task async + await round-trip
 *
 * Task returns value (42), status = OK.
 * ========================================================================= */

static void *task_return_42(void *arg) {
    (void)arg;
    int *result = (int *)malloc(sizeof(int));
    *result = 42;
    return result;
}

static volatile int t1_passed = 0;

static void t1_runner(void *arg) {
    (void)arg;
    sw_task_t task = sw_task_async(task_return_42, NULL);

    sw_task_result_t result = sw_task_await(&task, 5000);

    if (result.status == SW_TASK_OK && result.value != NULL) {
        int val = *(int *)result.value;
        free(result.value);
        t1_passed = (val == 42);
    }
}

static void test_task_async_await(void) {
    printf("\n=== T1: Task async + await ===\n");
    t1_passed = 0;

    sw_spawn(t1_runner, NULL);
    usleep(200000);

    TEST_CHECK("task_async_await", t1_passed);
}

/* =========================================================================
 * T2: Task crash
 *
 * Child sets exit_reason=7, await returns CRASH.
 * ========================================================================= */

static void *task_crash(void *arg) {
    (void)arg;
    sw_self()->exit_reason = 7;
    return NULL; /* Will exit with reason 7 */
}

static volatile int t2_passed = 0;

static void t2_runner(void *arg) {
    (void)arg;
    /* Trap exits so the link from spawn_link doesn't kill us */
    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    sw_task_t task = sw_task_async(task_crash, NULL);

    sw_task_result_t result = sw_task_await(&task, 5000);

    t2_passed = (result.status == SW_TASK_CRASH);
}

static void test_task_crash(void) {
    printf("\n=== T2: Task crash ===\n");
    t2_passed = 0;

    sw_spawn(t2_runner, NULL);
    usleep(200000);

    TEST_CHECK("task_crash", t2_passed);
}

/* =========================================================================
 * T3: Task timeout
 *
 * Child sleeps 500ms, await with 50ms timeout → TIMEOUT.
 * ========================================================================= */

static void *task_slow(void *arg) {
    (void)arg;
    usleep(500000); /* 500ms */
    return NULL;
}

static volatile int t3_passed = 0;

static void t3_runner(void *arg) {
    (void)arg;
    sw_task_t task = sw_task_async(task_slow, NULL);

    sw_task_result_t result = sw_task_await(&task, 50);

    t3_passed = (result.status == SW_TASK_TIMEOUT);

    /* Clean up the still-running task */
    sw_task_shutdown(&task);
}

static void test_task_timeout(void) {
    printf("\n=== T3: Task timeout ===\n");
    t3_passed = 0;

    sw_spawn(t3_runner, NULL);
    usleep(800000); /* Wait for slow task to finish too */

    TEST_CHECK("task_timeout", t3_passed);
}

/* =========================================================================
 * T4: sw_task_yield (non-blocking poll)
 *
 * Start task, sleep until it completes, then yield → OK.
 * ========================================================================= */

static void *task_quick(void *arg) {
    (void)arg;
    int *result = (int *)malloc(sizeof(int));
    *result = 99;
    return result;
}

static volatile int t4_passed = 0;

static void t4_runner(void *arg) {
    (void)arg;
    sw_task_t task = sw_task_async(task_quick, NULL);

    /* Wait for child to complete */
    usleep(100000);

    sw_task_result_t result = sw_task_yield(&task);

    if (result.status == SW_TASK_OK && result.value != NULL) {
        int val = *(int *)result.value;
        free(result.value);
        t4_passed = (val == 99);
    }
}

static void test_task_yield(void) {
    printf("\n=== T4: Task yield ===\n");
    t4_passed = 0;

    sw_spawn(t4_runner, NULL);
    usleep(300000);

    TEST_CHECK("task_yield", t4_passed);
}

/* =========================================================================
 * T5: Multiple concurrent tasks
 *
 * 4 tasks in parallel, all return correct values.
 * ========================================================================= */

static void *task_multiply(void *arg) {
    int input = (int)(intptr_t)arg;
    int *result = (int *)malloc(sizeof(int));
    *result = input * 10;
    return result;
}

static volatile int t5_passed = 0;

static void t5_runner(void *arg) {
    (void)arg;

    sw_task_t tasks[4];
    for (int i = 0; i < 4; i++) {
        tasks[i] = sw_task_async(task_multiply, (void *)(intptr_t)(i + 1));
    }

    int results[4] = {0};
    int all_ok = 1;
    for (int i = 0; i < 4; i++) {
        sw_task_result_t r = sw_task_await(&tasks[i], 5000);
        if (r.status == SW_TASK_OK && r.value) {
            results[i] = *(int *)r.value;
            free(r.value);
        } else {
            all_ok = 0;
        }
    }

    /* Verify: 1*10=10, 2*10=20, 3*10=30, 4*10=40 */
    if (all_ok) {
        t5_passed = (results[0] == 10 && results[1] == 20 &&
                     results[2] == 30 && results[3] == 40);
    }
}

static void test_task_concurrent(void) {
    printf("\n=== T5: Multiple concurrent tasks ===\n");
    t5_passed = 0;

    sw_spawn(t5_runner, NULL);
    usleep(300000);

    TEST_CHECK("task_concurrent", t5_passed);
}

/* =========================================================================
 * T6: Task inside GenServer
 *
 * handle_call spawns task, awaits, replies with result.
 * ========================================================================= */

static void *task_compute(void *arg) {
    int input = (int)(intptr_t)arg;
    int *result = (int *)malloc(sizeof(int));
    *result = input + 100;
    return result;
}

static void *gs_task_init(void *arg) {
    (void)arg;
    return NULL;
}

static sw_call_reply_t gs_task_handle_call(void *state, sw_process_t *from, void *request) {
    (void)from;
    int input = (int)(intptr_t)request;

    /* Spawn task, await result */
    sw_task_t task = sw_task_async(task_compute, (void *)(intptr_t)input);
    sw_task_result_t r = sw_task_await(&task, 5000);

    sw_call_reply_t reply = { .reply = r.value, .new_state = state };
    return reply;
}

static volatile int t6_passed = 0;

static void t6_caller(void *arg) {
    (void)arg;
    usleep(20000); /* Let GenServer start */

    void *result = sw_call("task_gs", (void *)(intptr_t)42, 5000);
    if (result) {
        int val = *(int *)result;
        free(result);
        t6_passed = (val == 142); /* 42 + 100 */
    }
}

static void test_task_in_genserver(void) {
    printf("\n=== T6: Task inside GenServer ===\n");
    t6_passed = 0;

    sw_gs_callbacks_t cbs = {
        .init = gs_task_init,
        .handle_call = gs_task_handle_call,
        .handle_cast = NULL,
        .handle_info = NULL,
        .terminate = NULL,
    };

    sw_genserver_start("task_gs", &cbs, NULL);
    sw_spawn(t6_caller, NULL);

    usleep(500000);

    sw_genserver_stop("task_gs");
    usleep(50000);

    TEST_CHECK("task_in_genserver", t6_passed);
}

/* =========================================================================
 * T7: sw_task_shutdown
 *
 * Cancel running task, verify child is killed.
 * ========================================================================= */

static _Atomic int t7_child_running = 0;

static void *task_long_running(void *arg) {
    (void)arg;
    atomic_store(&t7_child_running, 1);
    /* Check kill_flag periodically instead of one long sleep,
     * so shutdown takes effect promptly (cooperative scheduling) */
    for (int i = 0; i < 20 && !sw_self()->kill_flag; i++) {
        usleep(100000); /* 100ms chunks */
    }
    atomic_store(&t7_child_running, 0);
    return NULL;
}

static volatile int t7_passed = 0;

static void t7_runner(void *arg) {
    (void)arg;
    sw_task_t task = sw_task_async(task_long_running, NULL);

    usleep(50000); /* Let child start */

    int was_running = atomic_load(&t7_child_running);

    sw_task_shutdown(&task);

    usleep(100000); /* Let kill propagate */

    t7_passed = (was_running == 1 && task.completed == 1);
}

static void test_task_shutdown(void) {
    printf("\n=== T7: Task shutdown ===\n");
    t7_passed = 0;
    atomic_store(&t7_child_running, 0);

    sw_spawn(t7_runner, NULL);
    usleep(500000);

    TEST_CHECK("task_shutdown", t7_passed);
}

/* =========================================================================
 * E1: ETS Insert + Lookup
 *
 * 3 entries, correct values, miss returns NULL.
 * ========================================================================= */

static volatile int e1_passed = 0;

static void e1_runner(void *arg) {
    (void)arg;
    sw_ets_tid_t tid = sw_ets_new(SW_ETS_DEFAULT);

    int k1 = 1, k2 = 2, k3 = 3;
    int v1 = 10, v2 = 20, v3 = 30;
    int miss_key = 99;

    sw_ets_insert(tid, &k1, &v1);
    sw_ets_insert(tid, &k2, &v2);
    sw_ets_insert(tid, &k3, &v3);

    void *r1 = sw_ets_lookup(tid, &k1);
    void *r2 = sw_ets_lookup(tid, &k2);
    void *r3 = sw_ets_lookup(tid, &k3);
    void *r4 = sw_ets_lookup(tid, &miss_key);

    e1_passed = (r1 == &v1 && r2 == &v2 && r3 == &v3 && r4 == NULL);

    sw_ets_drop(tid);
}

static void test_ets_insert_lookup(void) {
    printf("\n=== E1: ETS Insert + Lookup ===\n");
    e1_passed = 0;

    sw_spawn(e1_runner, NULL);
    usleep(200000);

    TEST_CHECK("ets_insert_lookup", e1_passed);
}

/* =========================================================================
 * E2: ETS Set Replace
 *
 * Same key twice → second value wins, count=1.
 * ========================================================================= */

static volatile int e2_passed = 0;

static void e2_runner(void *arg) {
    (void)arg;
    sw_ets_tid_t tid = sw_ets_new(SW_ETS_DEFAULT);

    int key = 42;
    int v1 = 100, v2 = 200;

    sw_ets_insert(tid, &key, &v1);
    sw_ets_insert(tid, &key, &v2);

    void *result = sw_ets_lookup(tid, &key);
    int count = sw_ets_info_count(tid);

    e2_passed = (result == &v2 && count == 1);

    sw_ets_drop(tid);
}

static void test_ets_set_replace(void) {
    printf("\n=== E2: ETS Set Replace ===\n");
    e2_passed = 0;

    sw_spawn(e2_runner, NULL);
    usleep(200000);

    TEST_CHECK("ets_set_replace", e2_passed);
}

/* =========================================================================
 * E3: ETS Delete
 *
 * Delete middle entry, others unaffected.
 * ========================================================================= */

static volatile int e3_passed = 0;

static void e3_runner(void *arg) {
    (void)arg;
    sw_ets_tid_t tid = sw_ets_new(SW_ETS_DEFAULT);

    int k1 = 1, k2 = 2, k3 = 3;
    int v1 = 10, v2 = 20, v3 = 30;

    sw_ets_insert(tid, &k1, &v1);
    sw_ets_insert(tid, &k2, &v2);
    sw_ets_insert(tid, &k3, &v3);

    int del_result = sw_ets_delete(tid, &k2);

    void *r1 = sw_ets_lookup(tid, &k1);
    void *r2 = sw_ets_lookup(tid, &k2);
    void *r3 = sw_ets_lookup(tid, &k3);
    int count = sw_ets_info_count(tid);

    e3_passed = (del_result == 0 && r1 == &v1 && r2 == NULL && r3 == &v3 && count == 2);

    sw_ets_drop(tid);
}

static void test_ets_delete(void) {
    printf("\n=== E3: ETS Delete ===\n");
    e3_passed = 0;

    sw_spawn(e3_runner, NULL);
    usleep(200000);

    TEST_CHECK("ets_delete", e3_passed);
}

/* =========================================================================
 * E4: Concurrent reads
 *
 * 4 reader processes, 100 entries, all correct.
 * ========================================================================= */

static sw_ets_tid_t e4_tid = 0;
static int e4_keys[100];
static int e4_vals[100];
static _Atomic int e4_readers_ok = 0;

static void e4_reader(void *arg) {
    (void)arg;
    int ok = 1;

    for (int i = 0; i < 100; i++) {
        void *v = sw_ets_lookup(e4_tid, &e4_keys[i]);
        if (v != &e4_vals[i]) {
            ok = 0;
            break;
        }
    }

    if (ok) atomic_fetch_add(&e4_readers_ok, 1);
}

static void test_ets_concurrent_reads(void) {
    printf("\n=== E4: Concurrent reads ===\n");
    atomic_store(&e4_readers_ok, 0);

    /* Create PUBLIC table from main thread — no owner process needed,
     * avoids cooperative scheduling deadlock */
    for (int i = 0; i < 100; i++) {
        e4_keys[i] = i;
        e4_vals[i] = i * 7;
    }
    e4_tid = sw_ets_new(SW_ETS_DEFAULT);
    for (int i = 0; i < 100; i++) {
        sw_ets_insert(e4_tid, &e4_keys[i], &e4_vals[i]);
    }

    /* Spawn 4 readers from main thread — round-robin across schedulers */
    for (int i = 0; i < 4; i++) {
        sw_spawn(e4_reader, NULL);
    }

    usleep(500000);

    int readers = atomic_load(&e4_readers_ok);
    printf("  Readers OK: %d/4\n", readers);
    TEST_CHECK("ets_concurrent_reads", readers == 4);

    sw_ets_drop(e4_tid);
}

/* =========================================================================
 * E5: Protected access
 *
 * Non-owner can read, can't write.
 * ========================================================================= */

static sw_ets_tid_t e5_tid = 0;
static int e5_key = 42;
static int e5_val = 100;
static volatile int e5_reader_val = -1;   /* sentinel */
static volatile int e5_writer_result = 99; /* sentinel */

static void e5_reader_proc(void *arg) {
    sw_process_t *owner = (sw_process_t *)arg;
    void *v = sw_ets_lookup(e5_tid, &e5_key);
    e5_reader_val = v ? *(int *)v : 0;

    /* Signal owner we're done */
    int *done = (int *)malloc(sizeof(int));
    *done = 1;
    sw_send(owner, done);
}

static void e5_writer_proc(void *arg) {
    sw_process_t *owner = (sw_process_t *)arg;
    int other_val = 999;
    e5_writer_result = sw_ets_insert(e5_tid, &e5_key, &other_val);

    /* Signal owner we're done */
    int *done = (int *)malloc(sizeof(int));
    *done = 1;
    sw_send(owner, done);
}

static void e5_owner(void *arg) {
    (void)arg;
    sw_ets_opts_t opts = { SW_ETS_SET, SW_ETS_PROTECTED };
    e5_tid = sw_ets_new(opts);

    sw_ets_insert(e5_tid, &e5_key, &e5_val);

    /* Spawn reader and writer on different schedulers via spawn_link */
    sw_spawn_link(e5_reader_proc, sw_self());
    sw_spawn_link(e5_writer_proc, sw_self());

    /* Wait for both to complete, then exit (releases scheduler) */
    void *msg1 = sw_receive(5000);
    void *msg2 = sw_receive(5000);
    if (msg1) free(msg1);
    if (msg2) free(msg2);
}

static void test_ets_protected(void) {
    printf("\n=== E5: Protected access ===\n");
    e5_tid = 0;
    e5_reader_val = -1;
    e5_writer_result = 99;

    sw_spawn(e5_owner, NULL);
    usleep(500000);

    printf("  Reader got value: %d (expected 100)\n", e5_reader_val);
    printf("  Writer insert returned: %d (expected -1 = denied)\n", e5_writer_result);

    int passed = (e5_reader_val == 100 && e5_writer_result == -1);
    TEST_CHECK("ets_protected", passed);

    if (e5_tid) sw_ets_drop(e5_tid);
}

/* =========================================================================
 * E6: Private access
 *
 * Non-owner can't read or write.
 * ========================================================================= */

static sw_ets_tid_t e6_tid = 0;
static int e6_key = 42;
static int e6_val = 100;
static volatile int e6_read_result_is_null = -1; /* sentinel: -1=never ran */
static volatile int e6_write_result = 99;        /* sentinel: 99=never ran */

static void e6_intruder(void *arg) {
    sw_process_t *owner = (sw_process_t *)arg;

    void *v = sw_ets_lookup(e6_tid, &e6_key);
    e6_read_result_is_null = (v == NULL) ? 1 : 0;

    int other_val = 999;
    e6_write_result = sw_ets_insert(e6_tid, &e6_key, &other_val);

    /* Signal owner we're done */
    int *done = (int *)malloc(sizeof(int));
    *done = 1;
    sw_send(owner, done);
}

static void e6_owner(void *arg) {
    (void)arg;
    sw_ets_opts_t opts = { SW_ETS_SET, SW_ETS_PRIVATE };
    e6_tid = sw_ets_new(opts);

    sw_ets_insert(e6_tid, &e6_key, &e6_val);

    /* Spawn intruder on DIFFERENT scheduler via spawn_link */
    sw_spawn_link(e6_intruder, sw_self());

    /* Wait for intruder to signal completion */
    void *msg = sw_receive(5000);
    if (msg) free(msg);
}

static void test_ets_private(void) {
    printf("\n=== E6: Private access ===\n");
    e6_tid = 0;
    e6_read_result_is_null = -1;
    e6_write_result = 99;

    sw_spawn(e6_owner, NULL);
    usleep(500000);

    printf("  Intruder read returned NULL: %s (raw=%d)\n",
           e6_read_result_is_null == 1 ? "YES" : "NO", e6_read_result_is_null);
    printf("  Intruder write returned: %d (expected -1)\n", e6_write_result);

    int passed = (e6_read_result_is_null == 1 && e6_write_result == -1);
    TEST_CHECK("ets_private", passed);

    if (e6_tid) sw_ets_drop(e6_tid);
}

/* =========================================================================
 * E7: Owner exit cleanup
 *
 * Table auto-deleted when owner process dies.
 * ========================================================================= */

static volatile sw_ets_tid_t e7_tid = 0;

static void e7_owner(void *arg) {
    (void)arg;
    e7_tid = sw_ets_new(SW_ETS_DEFAULT);

    int key = 1, val = 42;
    sw_ets_insert(e7_tid, &key, &val);
    /* Process exits here — ETS table should be cleaned up */
}

static void test_ets_owner_exit(void) {
    printf("\n=== E7: Owner exit cleanup ===\n");
    e7_tid = 0;

    sw_spawn(e7_owner, NULL);
    usleep(300000); /* Wait for owner to exit and table to be cleaned */

    /* Try to look up the table — should fail */
    int count = sw_ets_info_count(e7_tid);
    printf("  Table info after owner exit: %d (expected -1 = gone)\n", count);

    TEST_CHECK("ets_owner_exit", count == -1);
}

/* =========================================================================
 * E8: Explicit drop
 *
 * sw_ets_drop, subsequent ops return error/NULL.
 * ========================================================================= */

static volatile int e8_passed = 0;

static void e8_runner(void *arg) {
    (void)arg;
    sw_ets_tid_t tid = sw_ets_new(SW_ETS_DEFAULT);

    int key = 1, val = 42;
    sw_ets_insert(tid, &key, &val);

    int drop_result = sw_ets_drop(tid);

    /* All subsequent ops should fail */
    void *lookup = sw_ets_lookup(tid, &key);
    int insert = sw_ets_insert(tid, &key, &val);
    int delete = sw_ets_delete(tid, &key);
    int count = sw_ets_info_count(tid);

    e8_passed = (drop_result == 0 &&
                 lookup == NULL &&
                 insert == -1 &&
                 delete == -1 &&
                 count == -1);
}

static void test_ets_drop(void) {
    printf("\n=== E8: Explicit drop ===\n");
    e8_passed = 0;

    sw_spawn(e8_runner, NULL);
    usleep(200000);

    TEST_CHECK("ets_drop", e8_passed);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n");
    printf("============================================\n");
    printf("  SwarmRT Phase 3: Task + ETS\n");
    printf("============================================\n");

    if (sw_init("phase3-test", 4) != 0) {
        fprintf(stderr, "Failed to initialize SwarmRT\n");
        return 1;
    }

    usleep(50000); /* Scheduler warm-up */

    /* Task tests */
    test_task_async_await();
    test_task_crash();
    test_task_timeout();
    test_task_yield();
    test_task_concurrent();
    test_task_in_genserver();
    test_task_shutdown();

    /* ETS tests */
    test_ets_insert_lookup();
    test_ets_set_replace();
    test_ets_delete();
    test_ets_concurrent_reads();
    test_ets_protected();
    test_ets_private();
    test_ets_owner_exit();
    test_ets_drop();

    sw_stats(0);

    printf("\n============================================\n");
    printf("  Results: %d passed, %d failed (of %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("============================================\n\n");

    sw_shutdown(0);
    return tests_failed > 0 ? 1 : 0;
}
