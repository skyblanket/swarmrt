/*
 * SwarmRT Phase 2: GenServer + Supervisor Tests
 *
 * Tests GenServer call/cast/stop and Supervisor restart strategies.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include "swarmrt_otp.h"

/* =========================================================================
 * Test 1: GenServer Counter (call + cast)
 *
 * A simple counter server:
 *   call("get")    → returns current count
 *   cast("inc")    → increments count (no reply)
 *   call("inc_ret") → increments and returns new count
 * ========================================================================= */

typedef struct {
    int count;
} counter_state_t;

static void *counter_init(void *arg) {
    counter_state_t *st = (counter_state_t *)malloc(sizeof(counter_state_t));
    st->count = arg ? *(int *)arg : 0;
    return st;
}

static sw_call_reply_t counter_handle_call(void *state, sw_process_t *from, void *request) {
    (void)from;
    counter_state_t *st = (counter_state_t *)state;
    char *cmd = (char *)request;

    sw_call_reply_t result;
    result.new_state = st;

    if (strcmp(cmd, "get") == 0) {
        int *reply = (int *)malloc(sizeof(int));
        *reply = st->count;
        result.reply = reply;
    } else if (strcmp(cmd, "inc_ret") == 0) {
        st->count++;
        int *reply = (int *)malloc(sizeof(int));
        *reply = st->count;
        result.reply = reply;
    } else {
        result.reply = NULL;
    }

    return result;
}

static void *counter_handle_cast(void *state, void *message) {
    counter_state_t *st = (counter_state_t *)state;
    char *cmd = (char *)message;

    if (strcmp(cmd, "inc") == 0) {
        st->count++;
    }

    return st;
}

static void counter_terminate(void *state, int reason) {
    (void)reason;
    free(state);
}

/* Caller process — runs call/cast operations against the counter */
static volatile int gs_test_passed = 0;

static void counter_caller(void *arg) {
    (void)arg;
    usleep(20000); /* Let GenServer start */

    /* Call "get" — should be 0 */
    int *val = (int *)sw_call("counter", "get", 2000);
    if (!val || *val != 0) { if (val) free(val); return; }
    free(val);

    /* Cast "inc" x3 (async, no reply) */
    sw_cast("counter", "inc");
    sw_cast("counter", "inc");
    sw_cast("counter", "inc");

    usleep(50000); /* Let casts process */

    /* Call "get" — should be 3 */
    val = (int *)sw_call("counter", "get", 2000);
    if (!val || *val != 3) { if (val) free(val); return; }
    free(val);

    /* Call "inc_ret" — should return 4 */
    val = (int *)sw_call("counter", "inc_ret", 2000);
    if (!val || *val != 4) { if (val) free(val); return; }
    free(val);

    /* Call "get" — should be 4 */
    val = (int *)sw_call("counter", "get", 2000);
    if (!val || *val != 4) { if (val) free(val); return; }
    free(val);

    gs_test_passed = 1;

    /* Stop the server */
    sw_genserver_stop("counter");
}

static void test_genserver_counter(void) {
    printf("\n=== Test: GenServer Counter ===\n");
    gs_test_passed = 0;

    sw_gs_callbacks_t cbs = {
        .init = counter_init,
        .handle_call = counter_handle_call,
        .handle_cast = counter_handle_cast,
        .handle_info = NULL,
        .terminate = counter_terminate,
    };

    sw_genserver_start("counter", &cbs, NULL);
    sw_spawn(counter_caller, NULL);

    usleep(500000);

    printf("  call(get)=0, cast(inc)x3, call(get)=3, call(inc_ret)=4: %s\n",
           gs_test_passed ? "YES" : "NO");
    printf("  GenServer Counter: %s\n", gs_test_passed ? "PASS" : "FAIL");
}

/* =========================================================================
 * Test 2: GenServer start_link (linked to caller)
 *
 * Verify that start_link creates a link — when the server dies,
 * the linked parent gets an EXIT signal.
 * ========================================================================= */

static volatile int gs_link_exit_received = 0;

static void *noop_init(void *arg) { return arg; }

static sw_call_reply_t crash_on_call(void *state, sw_process_t *from, void *request) {
    (void)from; (void)request;
    /* Crash: set exit_reason and stop */
    sw_self()->exit_reason = 99;
    sw_call_reply_t r = { .reply = NULL, .new_state = state };
    return r;
}

static void gs_link_parent(void *arg) {
    (void)arg;
    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    sw_gs_callbacks_t cbs = {
        .init = noop_init,
        .handle_call = crash_on_call,
    };

    sw_process_t *server = sw_genserver_start_link("crasher", &cbs, NULL);
    (void)server;

    usleep(20000); /* Let server start */

    /* This call will make the server crash */
    sw_genserver_stop("crasher");

    /* Should receive EXIT from the linked server */
    void *msg = sw_receive_tagged(SW_TAG_EXIT, 2000);
    if (msg) {
        gs_link_exit_received = 1;
        free(msg);
    }
}

static void test_genserver_link(void) {
    printf("\n=== Test: GenServer start_link ===\n");
    gs_link_exit_received = 0;

    sw_spawn(gs_link_parent, NULL);

    usleep(500000);

    printf("  Parent received EXIT from linked server: %s\n",
           gs_link_exit_received ? "YES" : "NO");
    printf("  GenServer start_link: %s\n",
           gs_link_exit_received ? "PASS" : "FAIL");
}

/* =========================================================================
 * Test 3: Supervisor one_for_one restart
 *
 * Supervisor starts 2 permanent workers. One crashes. Supervisor
 * restarts only the crashed one (one_for_one).
 * ========================================================================= */

static _Atomic int worker_a_starts = 0;
static _Atomic int worker_b_starts = 0;
static volatile int worker_a_should_crash = 1;

static void sup_worker_a(void *arg) {
    (void)arg;
    atomic_fetch_add(&worker_a_starts, 1);

    if (worker_a_should_crash) {
        worker_a_should_crash = 0; /* Only crash once */
        usleep(50000);
        sw_self()->exit_reason = 1; /* Abnormal exit → restart */
        return;
    }

    /* Second time: stay alive — use receive (not usleep) so we can be killed */
    sw_receive_any(2000, NULL);
}

static void sup_worker_b(void *arg) {
    (void)arg;
    atomic_fetch_add(&worker_b_starts, 1);
    /* Use receive (not usleep) so supervisor can kill us via condvar */
    sw_receive_any(2000, NULL);
}

static void test_supervisor_one_for_one(void) {
    printf("\n=== Test: Supervisor one_for_one ===\n");
    atomic_store(&worker_a_starts, 0);
    atomic_store(&worker_b_starts, 0);
    worker_a_should_crash = 1;

    sw_child_spec_t children[2] = {
        { .name = "worker_a", .start_func = sup_worker_a, .start_arg = NULL, .restart = SW_PERMANENT },
        { .name = "worker_b", .start_func = sup_worker_b, .start_arg = NULL, .restart = SW_PERMANENT },
    };

    sw_sup_spec_t spec = {
        .strategy = SW_ONE_FOR_ONE,
        .max_restarts = 5,
        .max_seconds = 10,
        .children = children,
        .num_children = 2,
    };

    sw_process_t *sup = sw_supervisor_start("test_sup", &spec);
    (void)sup;

    /* Wait for worker_a to crash and be restarted */
    usleep(500000);

    int a_starts = atomic_load(&worker_a_starts);
    int b_starts = atomic_load(&worker_b_starts);

    printf("  Worker A started %d times (expected 2 — initial + restart)\n", a_starts);
    printf("  Worker B started %d times (expected 1 — never restarted)\n", b_starts);
    printf("  Supervisor one_for_one: %s\n",
           (a_starts == 2 && b_starts == 1) ? "PASS" : "FAIL");

    /* Stop supervisor */
    sw_send_named("test_sup", SW_TAG_STOP, NULL);
    usleep(100000);
}

/* =========================================================================
 * Test 4: Supervisor circuit breaker (max_restarts exceeded)
 *
 * Child crashes repeatedly. After max_restarts, supervisor shuts down.
 * ========================================================================= */

static _Atomic int crasher_starts = 0;

static void always_crash(void *arg) {
    (void)arg;
    atomic_fetch_add(&crasher_starts, 1);
    usleep(20000); /* Brief pause before crashing */
    sw_self()->exit_reason = 1;
}

static void test_supervisor_circuit_breaker(void) {
    printf("\n=== Test: Supervisor Circuit Breaker ===\n");
    atomic_store(&crasher_starts, 0);

    sw_child_spec_t children[1] = {
        { .name = "crasher", .start_func = always_crash, .start_arg = NULL, .restart = SW_PERMANENT },
    };

    sw_sup_spec_t spec = {
        .strategy = SW_ONE_FOR_ONE,
        .max_restarts = 3,
        .max_seconds = 10,
        .children = children,
        .num_children = 1,
    };

    sw_supervisor_start("breaker_sup", &spec);

    /* Wait for crash loop and circuit breaker */
    usleep(1000000);

    int starts = atomic_load(&crasher_starts);

    /* Should start 4 times: initial + 3 restarts, then supervisor gives up */
    printf("  Crasher started %d times (expected 4 = 1 initial + 3 restarts)\n", starts);

    /* Supervisor should be dead — name unregistered */
    sw_process_t *sup = sw_whereis("breaker_sup");
    printf("  Supervisor alive: %s (expected: NO)\n", sup ? "YES" : "NO");
    printf("  Circuit breaker: %s\n",
           (starts == 4 && sup == NULL) ? "PASS" : "FAIL");
}

/* =========================================================================
 * Test 5: Supervisor with transient children
 *
 * Transient child exits normally (reason=0) → NOT restarted.
 * Transient child exits abnormally (reason!=0) → restarted.
 * ========================================================================= */

static _Atomic int transient_starts = 0;
static volatile int transient_should_fail = 1;

static void transient_worker(void *arg) {
    (void)arg;
    atomic_fetch_add(&transient_starts, 1);
    usleep(30000);

    if (transient_should_fail) {
        transient_should_fail = 0;
        sw_self()->exit_reason = 1; /* Abnormal → restart */
    }
    /* else: exit normally (reason=0) → NOT restarted */
}

static void test_supervisor_transient(void) {
    printf("\n=== Test: Supervisor Transient ===\n");
    atomic_store(&transient_starts, 0);
    transient_should_fail = 1;

    sw_child_spec_t children[1] = {
        { .name = "trans", .start_func = transient_worker, .start_arg = NULL, .restart = SW_TRANSIENT },
    };

    sw_sup_spec_t spec = {
        .strategy = SW_ONE_FOR_ONE,
        .max_restarts = 5,
        .max_seconds = 10,
        .children = children,
        .num_children = 1,
    };

    sw_supervisor_start("trans_sup", &spec);

    /* Wait: first start (abnormal exit) → restart → second start (normal exit) → done */
    usleep(500000);

    int starts = atomic_load(&transient_starts);

    printf("  Transient worker started %d times (expected 2)\n", starts);
    printf("  1st: abnormal exit → restarted, 2nd: normal exit → not restarted\n");
    printf("  Transient restart: %s\n", (starts == 2) ? "PASS" : "FAIL");

    sw_send_named("trans_sup", SW_TAG_STOP, NULL);
    usleep(100000);
}

/* =========================================================================
 * Test 6: GenServer call timeout
 *
 * Call a non-existent server → should return NULL (timeout or not found).
 * ========================================================================= */

static volatile int timeout_test_done = 0;
static volatile int timeout_test_passed = 0;

static void timeout_caller(void *arg) {
    (void)arg;
    /* Call a server that doesn't exist */
    void *result = sw_call("nonexistent_server", "hello", 100);
    timeout_test_passed = (result == NULL);
    timeout_test_done = 1;
}

static void test_genserver_timeout(void) {
    printf("\n=== Test: GenServer Call Timeout ===\n");
    timeout_test_done = 0;
    timeout_test_passed = 0;

    sw_spawn(timeout_caller, NULL);

    usleep(500000);

    printf("  Call to non-existent server returned NULL: %s\n",
           timeout_test_passed ? "YES" : "NO");
    printf("  Call timeout: %s\n", timeout_test_passed ? "PASS" : "FAIL");
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n");
    printf("============================================\n");
    printf("  SwarmRT Phase 2: GenServer + Supervisor\n");
    printf("============================================\n");

    if (sw_init("phase2-test", 4) != 0) {
        fprintf(stderr, "Failed to initialize SwarmRT\n");
        return 1;
    }

    usleep(50000); /* Scheduler warm-up */

    test_genserver_counter();
    test_genserver_link();
    test_genserver_timeout();
    test_supervisor_one_for_one();
    test_supervisor_transient();
    test_supervisor_circuit_breaker();

    sw_stats(0);

    printf("\n============================================\n");
    printf("  Phase 2 Tests Complete\n");
    printf("============================================\n\n");

    sw_shutdown(0);
    return 0;
}
