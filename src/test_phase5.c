/*
 * SwarmRT Phase 5: GenStateMachine + Process Groups
 *
 * 6 StateMachine + 6 PG = 12 tests.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include "swarmrt_phase5.h"
#include "swarmrt_task.h"
#include "swarmrt_ets.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_PASS(name) do { tests_passed++; printf("  [PASS] %s\n", name); } while(0)
#define TEST_FAIL(name) do { tests_failed++; printf("  [FAIL] %s\n", name); } while(0)
#define TEST_CHECK(name, cond) do { if (cond) TEST_PASS(name); else TEST_FAIL(name); } while(0)

/* =========================================================================
 * S1: Basic state machine — traffic light
 *
 * States: RED=0, GREEN=1, YELLOW=2
 * Cast "next" cycles through states.
 * Call "query" returns current state.
 * ========================================================================= */

enum { LIGHT_RED = 0, LIGHT_GREEN = 1, LIGHT_YELLOW = 2 };

static void *light_init(void *arg, int *state) {
    *state = LIGHT_RED;
    return arg;
}

static sw_sm_result_t light_handle(sw_sm_event_type_t type, void *event,
                                   int state, void *data, sw_process_t *from) {
    (void)from; (void)data;
    sw_sm_result_t r = { .action = SW_SM_KEEP_STATE, .new_data = data,
                         .next_state = state, .reply = NULL, .timeout_ms = 0 };

    if (type == SW_SM_CAST) {
        /* "next" cycles the light */
        int next = (state + 1) % 3;
        r.action = SW_SM_NEXT_STATE;
        r.next_state = next;
    } else if (type == SW_SM_CALL) {
        /* "query" returns current state */
        r.reply = (void *)(intptr_t)state;
    }
    return r;
}

static volatile int s1_passed = 0;

static void s1_runner(void *arg) {
    (void)arg;

    sw_sm_callbacks_t cbs = { .init = light_init, .handle_event = light_handle };
    sw_statemachine_start("s1_light", &cbs, NULL);
    usleep(10000);

    /* Should be RED initially */
    void *val = sw_sm_call("s1_light", NULL, 5000);
    int is_red = ((intptr_t)val == LIGHT_RED);

    /* Cycle: RED → GREEN */
    sw_sm_cast("s1_light", NULL);
    usleep(20000);
    val = sw_sm_call("s1_light", NULL, 5000);
    int is_green = ((intptr_t)val == LIGHT_GREEN);

    /* Cycle: GREEN → YELLOW */
    sw_sm_cast("s1_light", NULL);
    usleep(20000);
    val = sw_sm_call("s1_light", NULL, 5000);
    int is_yellow = ((intptr_t)val == LIGHT_YELLOW);

    s1_passed = is_red && is_green && is_yellow;
    printf("  States: RED=%d GREEN=%d YELLOW=%d\n", is_red, is_green, is_yellow);
    sw_sm_stop("s1_light");
}

static void test_sm_basic(void) {
    printf("\n=== S1: StateMachine basic (traffic light) ===\n");
    s1_passed = 0;
    sw_spawn(s1_runner, NULL);
    usleep(500000);
    TEST_CHECK("sm_basic", s1_passed);
}

/* =========================================================================
 * S2: State machine with state timeout
 *
 * Auto-transitions from ACTIVE to IDLE after 100ms timeout.
 * ========================================================================= */

enum { S2_IDLE = 0, S2_ACTIVE = 1 };

static void *s2_init(void *arg, int *state) {
    *state = S2_IDLE;
    return arg;
}

static sw_sm_result_t s2_handle(sw_sm_event_type_t type, void *event,
                                int state, void *data, sw_process_t *from) {
    (void)from; (void)event;
    sw_sm_result_t r = { .action = SW_SM_KEEP_STATE, .new_data = data,
                         .next_state = state, .reply = NULL, .timeout_ms = 0 };

    if (type == SW_SM_CAST && state == S2_IDLE) {
        /* Activate with timeout */
        r.action = SW_SM_NEXT_STATE;
        r.next_state = S2_ACTIVE;
        r.timeout_ms = 100;
    } else if (type == SW_SM_TIMEOUT && state == S2_ACTIVE) {
        /* Timeout: go back to idle */
        r.action = SW_SM_NEXT_STATE;
        r.next_state = S2_IDLE;
    } else if (type == SW_SM_CALL) {
        r.reply = (void *)(intptr_t)state;
    }
    return r;
}

static volatile int s2_passed = 0;

static void s2_runner(void *arg) {
    (void)arg;

    sw_sm_callbacks_t cbs = { .init = s2_init, .handle_event = s2_handle };
    sw_statemachine_start("s2_sm", &cbs, NULL);
    usleep(10000);

    /* Should be IDLE */
    void *val = sw_sm_call("s2_sm", NULL, 5000);
    int was_idle = ((intptr_t)val == S2_IDLE);

    /* Activate */
    sw_sm_cast("s2_sm", NULL);
    usleep(20000);
    val = sw_sm_call("s2_sm", NULL, 5000);
    int was_active = ((intptr_t)val == S2_ACTIVE);

    /* Wait for timeout */
    usleep(200000);
    val = sw_sm_call("s2_sm", NULL, 5000);
    int back_to_idle = ((intptr_t)val == S2_IDLE);

    s2_passed = was_idle && was_active && back_to_idle;
    printf("  Idle=%d Active=%d BackToIdle=%d\n", was_idle, was_active, back_to_idle);
    sw_sm_stop("s2_sm");
}

static void test_sm_timeout(void) {
    printf("\n=== S2: StateMachine state timeout ===\n");
    s2_passed = 0;
    sw_spawn(s2_runner, NULL);
    usleep(800000);
    TEST_CHECK("sm_timeout", s2_passed);
}

/* =========================================================================
 * S3: State machine stop from handler
 * ========================================================================= */

static volatile int s3_terminated = 0;

static void *s3_init(void *arg, int *state) {
    *state = 0;
    return arg;
}

static sw_sm_result_t s3_handle(sw_sm_event_type_t type, void *event,
                                int state, void *data, sw_process_t *from) {
    (void)from; (void)event;
    sw_sm_result_t r = { .action = SW_SM_KEEP_STATE, .new_data = data,
                         .next_state = state, .reply = NULL, .timeout_ms = 0 };

    if (type == SW_SM_CAST) {
        r.action = SW_SM_STOP;
    } else if (type == SW_SM_CALL) {
        r.reply = (void *)(intptr_t)42;
    }
    return r;
}

static void s3_terminate(int state, void *data, int reason) {
    (void)state; (void)data; (void)reason;
    s3_terminated = 1;
}

static volatile int s3_passed = 0;

static void s3_runner(void *arg) {
    (void)arg;

    sw_sm_callbacks_t cbs = { .init = s3_init, .handle_event = s3_handle,
                              .terminate = s3_terminate };
    sw_statemachine_start("s3_sm", &cbs, NULL);
    usleep(10000);

    /* Should be alive */
    void *val = sw_sm_call("s3_sm", NULL, 5000);
    int got_reply = ((intptr_t)val == 42);

    /* Send stop event */
    sw_sm_cast("s3_sm", NULL);
    usleep(100000);

    /* Should be dead */
    sw_process_t *p = sw_whereis("s3_sm");
    s3_passed = got_reply && (p == NULL) && s3_terminated;
    printf("  Reply=%d Dead=%d Terminated=%d\n", got_reply, p == NULL, s3_terminated);
}

static void test_sm_stop(void) {
    printf("\n=== S3: StateMachine stop from handler ===\n");
    s3_passed = 0;
    s3_terminated = 0;
    sw_spawn(s3_runner, NULL);
    usleep(500000);
    TEST_CHECK("sm_stop", s3_passed);
}

/* =========================================================================
 * S4: State machine with state data
 *
 * Connection FSM: DISCONNECTED → CONNECTING → CONNECTED
 * Data tracks retry count.
 * ========================================================================= */

enum { CONN_DISCONNECTED = 0, CONN_CONNECTING = 1, CONN_CONNECTED = 2 };

typedef struct {
    int retries;
    int connected;
} s4_data_t;

static void *s4_init(void *arg, int *state) {
    (void)arg;
    *state = CONN_DISCONNECTED;
    s4_data_t *d = (s4_data_t *)calloc(1, sizeof(s4_data_t));
    return d;
}

static sw_sm_result_t s4_handle(sw_sm_event_type_t type, void *event,
                                int state, void *data, sw_process_t *from) {
    (void)from; (void)event;
    s4_data_t *d = (s4_data_t *)data;
    sw_sm_result_t r = { .action = SW_SM_KEEP_STATE, .new_data = data,
                         .next_state = state, .reply = NULL, .timeout_ms = 0 };

    if (type == SW_SM_CAST) {
        if (state == CONN_DISCONNECTED) {
            d->retries++;
            r.action = SW_SM_NEXT_STATE;
            r.next_state = CONN_CONNECTING;
            r.timeout_ms = 50; /* Connect timeout */
        }
    } else if (type == SW_SM_TIMEOUT) {
        if (state == CONN_CONNECTING) {
            /* "Connection succeeded" */
            d->connected = 1;
            r.action = SW_SM_NEXT_STATE;
            r.next_state = CONN_CONNECTED;
        }
    } else if (type == SW_SM_CALL) {
        /* Return state + retries packed */
        r.reply = (void *)(intptr_t)(state * 100 + d->retries);
    }
    return r;
}

static void s4_terminate(int state, void *data, int reason) {
    (void)state; (void)reason;
    free(data);
}

static volatile int s4_passed = 0;

static void s4_runner(void *arg) {
    (void)arg;

    sw_sm_callbacks_t cbs = { .init = s4_init, .handle_event = s4_handle,
                              .terminate = s4_terminate };
    sw_statemachine_start("s4_conn", &cbs, NULL);
    usleep(10000);

    /* Disconnected, 0 retries */
    void *val = sw_sm_call("s4_conn", NULL, 5000);
    int v0 = (intptr_t)val; /* state=0, retries=0 → 0 */

    /* Trigger connect */
    sw_sm_cast("s4_conn", NULL);
    usleep(20000);

    /* Should be CONNECTING, 1 retry */
    val = sw_sm_call("s4_conn", NULL, 5000);
    int v1 = (intptr_t)val; /* state=1, retries=1 → 101 */

    /* Wait for connect timeout → CONNECTED */
    usleep(200000);
    val = sw_sm_call("s4_conn", NULL, 5000);
    int v2 = (intptr_t)val; /* state=2, retries=1 → 201 */

    s4_passed = (v0 == 0 && v1 == 101 && v2 == 201);
    printf("  Disconnected=%d Connecting=%d Connected=%d\n",
           v0 == 0, v1 == 101, v2 == 201);
    sw_sm_stop("s4_conn");
}

static void test_sm_data(void) {
    printf("\n=== S4: StateMachine with state data ===\n");
    s4_passed = 0;
    sw_spawn(s4_runner, NULL);
    usleep(800000);
    TEST_CHECK("sm_data", s4_passed);
}

/* =========================================================================
 * S5: State machine start_link
 * ========================================================================= */

static volatile int s5_passed = 0;

static void *s5_init(void *arg, int *state) { *state = 0; return arg; }
static sw_sm_result_t s5_handle(sw_sm_event_type_t type, void *event,
                                int state, void *data, sw_process_t *from) {
    (void)type; (void)event; (void)from;
    return (sw_sm_result_t){ .action = SW_SM_KEEP_STATE, .new_data = data,
                             .next_state = state, .reply = NULL, .timeout_ms = 0 };
}

static void s5_runner(void *arg) {
    (void)arg;
    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    sw_sm_callbacks_t cbs = { .init = s5_init, .handle_event = s5_handle };
    sw_process_t *sm = sw_statemachine_start_link("s5_sm", &cbs, NULL);
    usleep(10000);

    sw_sm_stop("s5_sm");
    usleep(50000);

    void *msg = sw_receive(1000);
    s5_passed = (sm != NULL);
    if (msg) free(msg);
}

static void test_sm_start_link(void) {
    printf("\n=== S5: StateMachine start_link ===\n");
    s5_passed = 0;
    sw_spawn(s5_runner, NULL);
    usleep(300000);
    TEST_CHECK("sm_start_link", s5_passed);
}

/* =========================================================================
 * S6: State machine call with reply
 * ========================================================================= */

static volatile int s6_passed = 0;

static void *s6_init(void *arg, int *state) { *state = 0; return arg; }

static sw_sm_result_t s6_handle(sw_sm_event_type_t type, void *event,
                                int state, void *data, sw_process_t *from) {
    (void)event; (void)from;
    sw_sm_result_t r = { .action = SW_SM_KEEP_STATE, .new_data = data,
                         .next_state = state, .reply = NULL, .timeout_ms = 0 };
    if (type == SW_SM_CALL) {
        /* Echo back state * 7 */
        r.reply = (void *)(intptr_t)(state * 7);
        r.action = SW_SM_NEXT_STATE;
        r.next_state = state + 1;
    }
    return r;
}

static void s6_runner(void *arg) {
    (void)arg;

    sw_sm_callbacks_t cbs = { .init = s6_init, .handle_event = s6_handle };
    sw_statemachine_start("s6_sm", &cbs, NULL);
    usleep(10000);

    void *v0 = sw_sm_call("s6_sm", NULL, 5000); /* state=0 → reply=0, next=1 */
    void *v1 = sw_sm_call("s6_sm", NULL, 5000); /* state=1 → reply=7, next=2 */
    void *v2 = sw_sm_call("s6_sm", NULL, 5000); /* state=2 → reply=14, next=3 */

    s6_passed = ((intptr_t)v0 == 0 && (intptr_t)v1 == 7 && (intptr_t)v2 == 14);
    printf("  Replies: %ld %ld %ld (expected 0 7 14)\n",
           (long)(intptr_t)v0, (long)(intptr_t)v1, (long)(intptr_t)v2);
    sw_sm_stop("s6_sm");
}

static void test_sm_call_reply(void) {
    printf("\n=== S6: StateMachine call with reply ===\n");
    s6_passed = 0;
    sw_spawn(s6_runner, NULL);
    usleep(400000);
    TEST_CHECK("sm_call_reply", s6_passed);
}

/* =========================================================================
 * P1: Process group join + dispatch
 * ========================================================================= */

static volatile int p1_received = 0;

static void p1_worker(void *arg) {
    (void)arg;
    sw_pg_join("p1_group", sw_self());

    uint64_t tag = 0;
    void *msg = sw_receive_any(5000, &tag);
    if (tag == SW_TAG_CAST) {
        __sync_fetch_and_add(&p1_received, 1);
    }
    if (msg) free(msg);
}

static volatile int p1_passed = 0;

static void p1_runner(void *arg) {
    (void)arg;

    /* Spawn 3 workers into group */
    for (int i = 0; i < 3; i++) {
        sw_spawn_link(p1_worker, NULL);
    }
    usleep(50000); /* Let them join */

    uint32_t count = sw_pg_count("p1_group");

    /* Dispatch to all */
    sw_pg_dispatch("p1_group", SW_TAG_CAST, NULL);
    usleep(200000);

    p1_passed = (count == 3 && p1_received == 3);
    printf("  Group count: %u (expected 3), Received: %d (expected 3)\n",
           count, p1_received);
}

static void test_pg_join_dispatch(void) {
    printf("\n=== P1: PG join + dispatch ===\n");
    p1_received = 0;
    p1_passed = 0;
    sw_spawn(p1_runner, NULL);
    usleep(500000);
    TEST_CHECK("pg_join_dispatch", p1_passed);
}

/* =========================================================================
 * P2: Process group leave
 * ========================================================================= */

static volatile int p2_passed = 0;

static void p2_worker(void *arg) {
    (void)arg;
    sw_pg_join("p2_group", sw_self());
    sw_receive(100); /* Short timeout — runner checks fast, then we exit */
}

static void p2_runner(void *arg) {
    (void)arg;

    sw_process_t *w1 = sw_spawn_link(p2_worker, NULL);
    sw_spawn_link(p2_worker, NULL);
    usleep(50000);

    uint32_t before = sw_pg_count("p2_group");
    sw_pg_leave("p2_group", w1);
    uint32_t after = sw_pg_count("p2_group");

    p2_passed = (before == 2 && after == 1);
    printf("  Before: %u, After leave: %u\n", before, after);
}

static void test_pg_leave(void) {
    printf("\n=== P2: PG leave ===\n");
    p2_passed = 0;
    sw_spawn(p2_runner, NULL);
    usleep(300000);
    TEST_CHECK("pg_leave", p2_passed);
}

/* =========================================================================
 * P3: Process group members list
 * ========================================================================= */

static volatile int p3_passed = 0;

static void p3_worker(void *arg) {
    (void)arg;
    sw_pg_join("p3_group", sw_self());
    sw_receive(100);
}

static void p3_runner(void *arg) {
    (void)arg;

    for (int i = 0; i < 3; i++) {
        sw_spawn_link(p3_worker, NULL);
    }
    /* Also join self to test mixed membership */
    sw_pg_join("p3_group", sw_self());
    usleep(50000);

    sw_process_t *members[16];
    int count = sw_pg_members("p3_group", members, 16);

    p3_passed = (count == 4);
    printf("  Members: %d (expected 4)\n", count);
}

static void test_pg_members(void) {
    printf("\n=== P3: PG members list ===\n");
    p3_passed = 0;
    sw_spawn(p3_runner, NULL);
    usleep(300000);
    TEST_CHECK("pg_members", p3_passed);
}

/* =========================================================================
 * P4: Process group auto-cleanup on death
 * ========================================================================= */

static volatile int p4_passed = 0;

static void p4_mortal(void *arg) {
    (void)arg;
    sw_pg_join("p4_group", sw_self());
    /* Exit immediately */
}

static void p4_runner(void *arg) {
    (void)arg;

    /* Spawn two processes that join then exit immediately */
    sw_spawn_link(p4_mortal, NULL);
    sw_spawn_link(p4_mortal, NULL);
    usleep(200000); /* Let both run, exit, and cleanup */

    uint32_t count = sw_pg_count("p4_group");
    p4_passed = (count == 0);
    printf("  Count after deaths: %u (expected 0)\n", count);
}

static void test_pg_cleanup(void) {
    printf("\n=== P4: PG auto-cleanup on death ===\n");
    p4_passed = 0;
    sw_spawn(p4_runner, NULL);
    usleep(300000);
    TEST_CHECK("pg_cleanup", p4_passed);
}

/* =========================================================================
 * P5: Multiple groups
 * ========================================================================= */

static volatile int p5_passed = 0;

static void p5_multi_worker(void *arg) {
    (void)arg;
    sw_pg_join("p5_alpha", sw_self());
    sw_pg_join("p5_beta", sw_self());
    sw_receive(100);
}

static void p5_runner(void *arg) {
    (void)arg;

    sw_spawn_link(p5_multi_worker, NULL);
    sw_spawn_link(p5_multi_worker, NULL);
    usleep(50000);

    uint32_t alpha = sw_pg_count("p5_alpha");
    uint32_t beta = sw_pg_count("p5_beta");

    p5_passed = (alpha == 2 && beta == 2);
    printf("  Alpha: %u, Beta: %u (expected 2, 2)\n", alpha, beta);
}

static void test_pg_multiple_groups(void) {
    printf("\n=== P5: PG multiple groups ===\n");
    p5_passed = 0;
    sw_spawn(p5_runner, NULL);
    usleep(300000);
    TEST_CHECK("pg_multiple_groups", p5_passed);
}

/* =========================================================================
 * P6: Dispatch to empty group
 * ========================================================================= */

static void test_pg_empty_dispatch(void) {
    printf("\n=== P6: PG dispatch to empty/nonexistent group ===\n");
    int sent = sw_pg_dispatch("nonexistent_group", SW_TAG_CAST, NULL);
    TEST_CHECK("pg_empty_dispatch", sent == 0);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("============================================\n");
    printf("  SwarmRT Phase 5: StateMachine + PG\n");
    printf("============================================\n");

    if (sw_init("phase5-test", 4) != 0) {
        fprintf(stderr, "Failed to initialize SwarmRT\n");
        return 1;
    }

    /* StateMachine tests */
    test_sm_basic();
    test_sm_timeout();
    test_sm_stop();
    test_sm_data();
    test_sm_start_link();
    test_sm_call_reply();

    /* Process Group tests */
    test_pg_join_dispatch();
    test_pg_leave();
    test_pg_members();
    test_pg_cleanup();
    test_pg_multiple_groups();
    test_pg_empty_dispatch();

    sw_stats(0);
    sw_shutdown(0);

    printf("\n============================================\n");
    printf("  Results: %d passed, %d failed (of %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
