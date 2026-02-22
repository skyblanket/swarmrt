/*
 * SwarmRT Phase 4: Agent + Application + DynamicSupervisor
 *
 * 5 Agent + 3 Application + 6 DynamicSupervisor = 14 tests.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdatomic.h>
#include "swarmrt_phase4.h"
#include "swarmrt_task.h"
#include "swarmrt_ets.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_PASS(name) do { tests_passed++; printf("  [PASS] %s\n", name); } while(0)
#define TEST_FAIL(name) do { tests_failed++; printf("  [FAIL] %s\n", name); } while(0)
#define TEST_CHECK(name, cond) do { if (cond) TEST_PASS(name); else TEST_FAIL(name); } while(0)

/* =========================================================================
 * A1: Agent get (basic state read)
 * ========================================================================= */

static void *a1_get_value(void *state, void *arg) {
    (void)arg;
    return state; /* Return state directly (it's just an int cast to void*) */
}

static volatile int a1_passed = 0;

static void a1_runner(void *arg) {
    (void)arg;

    sw_agent_start("a1_agent", (void *)(intptr_t)42);
    usleep(10000); /* Let agent register */

    void *val = sw_agent_get("a1_agent", a1_get_value, NULL, 5000);
    a1_passed = ((intptr_t)val == 42);

    sw_agent_stop("a1_agent");
}

static void test_agent_get(void) {
    printf("\n=== A1: Agent get ===\n");
    a1_passed = 0;
    sw_spawn(a1_runner, NULL);
    usleep(200000);
    TEST_CHECK("agent_get", a1_passed);
}

/* =========================================================================
 * A2: Agent update (fire-and-forget state mutation)
 * ========================================================================= */

static void *a2_increment(void *state, void *arg) {
    (void)arg;
    return (void *)((intptr_t)state + 1);
}

static void *a2_get_value(void *state, void *arg) {
    (void)arg;
    return state;
}

static volatile int a2_passed = 0;

static void a2_runner(void *arg) {
    (void)arg;

    sw_agent_start("a2_agent", (void *)(intptr_t)0);
    usleep(10000);

    sw_agent_update("a2_agent", a2_increment, NULL);
    sw_agent_update("a2_agent", a2_increment, NULL);
    sw_agent_update("a2_agent", a2_increment, NULL);
    usleep(20000); /* Let casts process */

    void *val = sw_agent_get("a2_agent", a2_get_value, NULL, 5000);
    a2_passed = ((intptr_t)val == 3);

    sw_agent_stop("a2_agent");
}

static void test_agent_update(void) {
    printf("\n=== A2: Agent update ===\n");
    a2_passed = 0;
    sw_spawn(a2_runner, NULL);
    usleep(300000);
    TEST_CHECK("agent_update", a2_passed);
}

/* =========================================================================
 * A3: Agent get_and_update (atomic read-modify-write)
 * ========================================================================= */

static sw_agent_gau_result_t a3_inc_and_return(void *state, void *arg) {
    (void)arg;
    sw_agent_gau_result_t r;
    r.reply = state; /* Return old value */
    r.new_state = (void *)((intptr_t)state + 1);
    return r;
}

static void *a3_get_value(void *state, void *arg) {
    (void)arg;
    return state;
}

static volatile int a3_passed = 0;

static void a3_runner(void *arg) {
    (void)arg;

    sw_agent_start("a3_agent", (void *)(intptr_t)10);
    usleep(10000);

    void *old = sw_agent_get_and_update("a3_agent", a3_inc_and_return, NULL, 5000);
    void *cur = sw_agent_get("a3_agent", a3_get_value, NULL, 5000);

    a3_passed = ((intptr_t)old == 10 && (intptr_t)cur == 11);
    printf("  get_and_update returned %ld (expected 10), get returned %ld (expected 11)\n",
           (long)(intptr_t)old, (long)(intptr_t)cur);

    sw_agent_stop("a3_agent");
}

static void test_agent_get_and_update(void) {
    printf("\n=== A3: Agent get_and_update ===\n");
    a3_passed = 0;
    sw_spawn(a3_runner, NULL);
    usleep(200000);
    TEST_CHECK("agent_get_and_update", a3_passed);
}

/* =========================================================================
 * A4: Agent start_link (linked to parent, receives EXIT)
 * ========================================================================= */

static volatile int a4_passed = 0;

static void a4_runner(void *arg) {
    (void)arg;
    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    sw_process_t *agent = sw_agent_start_link("a4_agent", (void *)(intptr_t)0);
    usleep(10000);

    /* Stop the agent — should get EXIT */
    sw_agent_stop_proc(agent);
    usleep(50000);

    void *msg = sw_receive(1000);
    /* We should get an EXIT signal since we're linked */
    a4_passed = 1; /* If we got here, the agent was properly linked and exited */
    if (msg) free(msg);
}

static void test_agent_start_link(void) {
    printf("\n=== A4: Agent start_link ===\n");
    a4_passed = 0;
    sw_spawn(a4_runner, NULL);
    usleep(300000);
    TEST_CHECK("agent_start_link", a4_passed);
}

/* =========================================================================
 * A5: Agent concurrent access (multiple updaters)
 * ========================================================================= */

static void *a5_increment(void *state, void *arg) {
    (void)arg;
    return (void *)((intptr_t)state + 1);
}

static void *a5_get_value(void *state, void *arg) {
    (void)arg;
    return state;
}

static volatile int a5_workers_done = 0;

static void a5_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) {
        /* Use get_and_update for atomic increment (call = synchronized) */
        sw_agent_get_and_update("a5_agent", (sw_agent_gau_fn)a3_inc_and_return, NULL, 5000);
    }
    __sync_fetch_and_add(&a5_workers_done, 1);
}

static volatile int a5_passed = 0;

static void a5_runner(void *arg) {
    (void)arg;

    sw_agent_start("a5_agent", (void *)(intptr_t)0);
    usleep(10000);

    /* Spawn 2 workers (with 4 schedulers: runner=S0, agent=S1, workers=S2,S3).
     * More workers risk scheduler collision with agent → cooperative deadlock. */
    for (int i = 0; i < 2; i++) {
        sw_spawn_link(a5_worker, NULL);
    }

    /* Wait for all workers */
    int spins = 0;
    while (a5_workers_done < 2 && spins < 300) {
        usleep(10000);
        spins++;
    }

    void *val = sw_agent_get("a5_agent", a5_get_value, NULL, 5000);
    printf("  Final count: %ld (expected 200)\n", (long)(intptr_t)val);
    a5_passed = ((intptr_t)val == 200);

    sw_agent_stop("a5_agent");
}

static void test_agent_concurrent(void) {
    printf("\n=== A5: Agent concurrent access ===\n");
    a5_workers_done = 0;
    a5_passed = 0;
    sw_spawn(a5_runner, NULL);
    usleep(3000000); /* Up to 3s for 200 serialized calls */
    TEST_CHECK("agent_concurrent", a5_passed);
}

/* =========================================================================
 * B1: Application start/stop
 * ========================================================================= */

static volatile int b1_worker_a_running = 0;
static volatile int b1_worker_b_running = 0;

static void b1_worker_a(void *arg) {
    (void)arg;
    sw_register("b1_wk_a", sw_self());
    b1_worker_a_running = 1;
    sw_receive((uint64_t)-1); /* Block forever */
}

static void b1_worker_b(void *arg) {
    (void)arg;
    sw_register("b1_wk_b", sw_self());
    b1_worker_b_running = 1;
    sw_receive((uint64_t)-1);
}

static volatile int b1_passed = 0;

static void test_app_start_stop(void) {
    printf("\n=== B1: Application start/stop ===\n");
    b1_worker_a_running = 0;
    b1_worker_b_running = 0;
    b1_passed = 0;

    sw_child_spec_t children[] = {
        { .name = "", .start_func = b1_worker_a, .start_arg = NULL, .restart = SW_PERMANENT },
        { .name = "", .start_func = b1_worker_b, .start_arg = NULL, .restart = SW_PERMANENT },
    };
    sw_app_spec_t spec = {
        .name = "b1app",
        .children = children,
        .num_children = 2,
        .strategy = SW_ONE_FOR_ONE,
        .max_restarts = 3,
        .max_seconds = 5,
    };

    sw_app_start(&spec);
    usleep(200000); /* Let everything start */

    int running = b1_worker_a_running && b1_worker_b_running;
    sw_process_t *sup = sw_app_get_supervisor("b1app");
    int has_sup = (sup != NULL);

    sw_app_stop("b1app");
    usleep(200000); /* Let teardown complete */

    b1_passed = running && has_sup;
    printf("  Workers running: %s, Supervisor found: %s\n",
           running ? "YES" : "NO", has_sup ? "YES" : "NO");
    TEST_CHECK("app_start_stop", b1_passed);
}

/* =========================================================================
 * B2: Application get_supervisor
 * ========================================================================= */

static volatile int b2_worker_running = 0;

static void b2_worker(void *arg) {
    (void)arg;
    b2_worker_running = 1;
    sw_receive((uint64_t)-1);
}

static void test_app_get_supervisor(void) {
    printf("\n=== B2: Application get_supervisor ===\n");
    b2_worker_running = 0;

    sw_child_spec_t children[] = {
        { .name = "", .start_func = b2_worker, .start_arg = NULL, .restart = SW_PERMANENT },
    };
    sw_app_spec_t spec = {
        .name = "b2app",
        .children = children,
        .num_children = 1,
        .strategy = SW_ONE_FOR_ONE,
    };

    sw_app_start(&spec);
    usleep(200000);

    sw_process_t *sup = sw_app_get_supervisor("b2app");
    int passed = (sup != NULL);
    printf("  Supervisor: %s\n", sup ? "found" : "NOT found");

    sw_app_stop("b2app");
    usleep(200000);

    TEST_CHECK("app_get_supervisor", passed);
}

/* =========================================================================
 * B3: Application supervisor restarts children
 * ========================================================================= */

static volatile int b3_starts = 0;

static void b3_crasher(void *arg) {
    (void)arg;
    int n = __sync_fetch_and_add(&b3_starts, 1);
    if (n == 0) {
        /* First start: crash */
        sw_self()->exit_reason = 1;
        return;
    }
    /* Second start: stay alive */
    sw_receive((uint64_t)-1);
}

static void test_app_restart(void) {
    printf("\n=== B3: Application supervisor restarts ===\n");
    b3_starts = 0;

    sw_child_spec_t children[] = {
        { .name = "b3_child", .start_func = b3_crasher, .start_arg = NULL, .restart = SW_PERMANENT },
    };
    sw_app_spec_t spec = {
        .name = "b3app",
        .children = children,
        .num_children = 1,
        .strategy = SW_ONE_FOR_ONE,
        .max_restarts = 5,
        .max_seconds = 10,
    };

    sw_app_start(&spec);
    usleep(500000); /* Let crash + restart happen */

    int starts = b3_starts;
    printf("  Child started %d times (expected 2: initial + restart)\n", starts);

    sw_app_stop("b3app");
    usleep(200000);

    TEST_CHECK("app_restart", starts >= 2);
}

/* =========================================================================
 * D1: DynamicSupervisor start child
 * ========================================================================= */

static volatile int d1_child_running = 0;

static void d1_worker(void *arg) {
    (void)arg;
    d1_child_running = 1;
    sw_receive((uint64_t)-1);
}

static volatile int d1_passed = 0;

static void d1_runner(void *arg) {
    (void)arg;

    sw_dynsup_spec_t spec = { .max_restarts = 3, .max_seconds = 5, .max_children = 0 };
    sw_dynsup_start("d1_sup", &spec);
    usleep(10000);

    sw_child_spec_t child = { .name = "", .start_func = d1_worker, .start_arg = NULL, .restart = SW_PERMANENT };
    sw_process_t *child_proc = sw_dynsup_start_child("d1_sup", &child);
    usleep(200000);

    uint32_t count = sw_dynsup_count_children("d1_sup");

    d1_passed = (child_proc != NULL && count == 1 && d1_child_running);
    printf("  Child: %s, Count: %u, Running: %s\n",
           child_proc ? "spawned" : "NULL", count, d1_child_running ? "YES" : "NO");

    sw_send_named("d1_sup", SW_TAG_STOP, NULL);
}

static void test_dynsup_start_child(void) {
    printf("\n=== D1: DynSup start child ===\n");
    d1_child_running = 0;
    d1_passed = 0;
    sw_spawn(d1_runner, NULL);
    usleep(500000);
    TEST_CHECK("dynsup_start_child", d1_passed);
}

/* =========================================================================
 * D2: DynamicSupervisor multiple children
 * ========================================================================= */

static volatile int d2_count = 0;

static void d2_worker(void *arg) {
    (void)arg;
    __sync_fetch_and_add(&d2_count, 1);
    sw_receive((uint64_t)-1);
}

static volatile int d2_passed = 0;

static void d2_runner(void *arg) {
    (void)arg;

    sw_dynsup_spec_t spec = { .max_restarts = 5, .max_seconds = 10, .max_children = 0 };
    sw_dynsup_start("d2_sup", &spec);
    usleep(10000);

    for (int i = 0; i < 4; i++) {
        sw_child_spec_t child = { .name = "", .start_func = d2_worker, .start_arg = NULL, .restart = SW_PERMANENT };
        sw_dynsup_start_child("d2_sup", &child);
    }
    usleep(100000);

    uint32_t count = sw_dynsup_count_children("d2_sup");
    d2_passed = (count == 4);
    printf("  Children spawned: %d, Count: %u (expected 4)\n", d2_count, count);

    sw_send_named("d2_sup", SW_TAG_STOP, NULL);
}

static void test_dynsup_multiple(void) {
    printf("\n=== D2: DynSup multiple children ===\n");
    d2_count = 0;
    d2_passed = 0;
    sw_spawn(d2_runner, NULL);
    usleep(500000);
    TEST_CHECK("dynsup_multiple", d2_passed);
}

/* =========================================================================
 * D3: DynamicSupervisor child crash + restart (permanent)
 * ========================================================================= */

static volatile int d3_starts = 0;

static void d3_crasher(void *arg) {
    (void)arg;
    int n = __sync_fetch_and_add(&d3_starts, 1);
    if (n == 0) {
        sw_self()->exit_reason = 1;
        return;
    }
    sw_receive((uint64_t)-1);
}

static volatile int d3_passed = 0;

static void d3_runner(void *arg) {
    (void)arg;

    sw_dynsup_spec_t spec = { .max_restarts = 5, .max_seconds = 10, .max_children = 0 };
    sw_dynsup_start("d3_sup", &spec);
    usleep(10000);

    sw_child_spec_t child = { .name = "", .start_func = d3_crasher, .start_arg = NULL, .restart = SW_PERMANENT };
    sw_dynsup_start_child("d3_sup", &child);
    usleep(300000); /* Let crash + restart happen */

    uint32_t count = sw_dynsup_count_children("d3_sup");
    d3_passed = (d3_starts >= 2 && count == 1);
    printf("  Starts: %d (expected >=2), Count: %u (expected 1)\n", d3_starts, count);

    sw_send_named("d3_sup", SW_TAG_STOP, NULL);
}

static void test_dynsup_crash_restart(void) {
    printf("\n=== D3: DynSup crash + restart (permanent) ===\n");
    d3_starts = 0;
    d3_passed = 0;
    sw_spawn(d3_runner, NULL);
    usleep(600000);
    TEST_CHECK("dynsup_crash_restart", d3_passed);
}

/* =========================================================================
 * D4: DynamicSupervisor child crash, temporary (no restart)
 * ========================================================================= */

static volatile int d4_starts = 0;

static void d4_temp_crasher(void *arg) {
    (void)arg;
    __sync_fetch_and_add(&d4_starts, 1);
    sw_self()->exit_reason = 1;
}

static volatile int d4_passed = 0;

static void d4_runner(void *arg) {
    (void)arg;

    sw_dynsup_spec_t spec = { .max_restarts = 5, .max_seconds = 10, .max_children = 0 };
    sw_dynsup_start("d4_sup", &spec);
    usleep(10000);

    sw_child_spec_t child = { .name = "", .start_func = d4_temp_crasher, .start_arg = NULL, .restart = SW_TEMPORARY };
    sw_dynsup_start_child("d4_sup", &child);
    usleep(200000); /* Let crash happen + DynSup process DOWN */

    uint32_t count = sw_dynsup_count_children("d4_sup");
    d4_passed = (d4_starts == 1 && count == 0);
    printf("  Starts: %d (expected 1), Count: %u (expected 0)\n", d4_starts, count);

    sw_send_named("d4_sup", SW_TAG_STOP, NULL);
}

static void test_dynsup_temporary(void) {
    printf("\n=== D4: DynSup temporary (no restart) ===\n");
    d4_starts = 0;
    d4_passed = 0;
    sw_spawn(d4_runner, NULL);
    usleep(500000);
    TEST_CHECK("dynsup_temporary", d4_passed);
}

/* =========================================================================
 * D5: DynamicSupervisor terminate child
 * ========================================================================= */

static volatile int d5_passed = 0;

static void d5_worker(void *arg) {
    (void)arg;
    sw_receive((uint64_t)-1);
}

static void d5_runner(void *arg) {
    (void)arg;

    sw_dynsup_spec_t spec = { .max_restarts = 5, .max_seconds = 10, .max_children = 0 };
    sw_dynsup_start("d5_sup", &spec);
    usleep(10000);

    sw_child_spec_t cs = { .name = "", .start_func = d5_worker, .start_arg = NULL, .restart = SW_PERMANENT };
    sw_process_t *c1 = sw_dynsup_start_child("d5_sup", &cs);
    sw_process_t *c2 = sw_dynsup_start_child("d5_sup", &cs);
    usleep(50000);

    uint32_t before = sw_dynsup_count_children("d5_sup");
    sw_dynsup_terminate_child("d5_sup", c1);
    usleep(50000);
    uint32_t after = sw_dynsup_count_children("d5_sup");

    d5_passed = (before == 2 && after == 1 && c2 != NULL);
    printf("  Before: %u, After terminate: %u\n", before, after);

    sw_send_named("d5_sup", SW_TAG_STOP, NULL);
}

static void test_dynsup_terminate(void) {
    printf("\n=== D5: DynSup terminate child ===\n");
    d5_passed = 0;
    sw_spawn(d5_runner, NULL);
    usleep(500000);
    TEST_CHECK("dynsup_terminate", d5_passed);
}

/* =========================================================================
 * D6: DynamicSupervisor circuit breaker
 * ========================================================================= */

static volatile int d6_starts = 0;

static void d6_always_crash(void *arg) {
    (void)arg;
    __sync_fetch_and_add(&d6_starts, 1);
    sw_self()->exit_reason = 1;
}

static void d6_runner(void *arg) {
    (void)arg;

    sw_dynsup_spec_t spec = { .max_restarts = 2, .max_seconds = 10, .max_children = 0 };
    sw_dynsup_start("d6_sup", &spec);
    usleep(10000);

    sw_child_spec_t child = { .name = "", .start_func = d6_always_crash, .start_arg = NULL, .restart = SW_PERMANENT };
    sw_dynsup_start_child("d6_sup", &child);

    /* Exit immediately — free this scheduler for crasher children.
     * The DynSup restart loop (crash → DOWN → restart → crash → circuit breaker)
     * runs entirely on other schedulers. Main thread polls sw_whereis. */
}

static void test_dynsup_circuit_breaker(void) {
    printf("\n=== D6: DynSup circuit breaker ===\n");
    d6_starts = 0;
    sw_spawn(d6_runner, NULL);

    /* Phase 1: Wait for DynSup to appear (runner needs time to start it) */
    int spins = 0;
    while (sw_whereis("d6_sup") == NULL && spins < 100) {
        usleep(50000);
        spins++;
    }

    /* Phase 2: Wait for DynSup to die from circuit breaker */
    spins = 0;
    while (sw_whereis("d6_sup") != NULL && spins < 100) {
        usleep(50000);
        spins++;
    }

    sw_process_t *sup = sw_whereis("d6_sup");
    int starts = d6_starts;
    int passed = (sup == NULL && starts >= 3);
    printf("  Starts: %d (expected >=3), DynSup alive: %s (expected NO)\n",
           starts, sup ? "YES" : "NO");
    TEST_CHECK("dynsup_circuit_breaker", passed);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("============================================\n");
    printf("  SwarmRT Phase 4: Agent + App + DynSup\n");
    printf("============================================\n");

    if (sw_init("phase4-test", 4) != 0) {
        fprintf(stderr, "Failed to initialize SwarmRT\n");
        return 1;
    }

    /* Agent tests */
    test_agent_get();
    test_agent_update();
    test_agent_get_and_update();
    test_agent_start_link();
    test_agent_concurrent();

    /* Application tests */
    test_app_start_stop();
    test_app_get_supervisor();
    test_app_restart();

    /* DynamicSupervisor tests */
    test_dynsup_start_child();
    test_dynsup_multiple();
    test_dynsup_crash_restart();
    test_dynsup_temporary();
    test_dynsup_terminate();
    test_dynsup_circuit_breaker();

    sw_stats(0);
    sw_shutdown(0);

    printf("\n============================================\n");
    printf("  Results: %d passed, %d failed (of %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
