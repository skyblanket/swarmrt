/*
 * SwarmRT Phase 7: Hot Code Reload Tests
 *
 * 5 tests: register, upgrade, rollback, notification, version tracking.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include "swarmrt_hotload.h"
#include "swarmrt_otp.h"
#include "swarmrt_ets.h"
#include "swarmrt_phase5.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_PASS(name) do { tests_passed++; printf("  [PASS] %s\n", name); } while(0)
#define TEST_FAIL(name) do { tests_failed++; printf("  [FAIL] %s\n", name); } while(0)
#define TEST_CHECK(name, cond) do { if (cond) TEST_PASS(name); else TEST_FAIL(name); } while(0)

/* =========================================================================
 * H1: Module register + spawn
 * ========================================================================= */

static volatile int h1_worker_ran = 0;

static void h1_worker_v1(void *arg) {
    (void)arg;
    h1_worker_ran = 1;
    sw_receive(500);
}

static volatile int h1_passed = 0;

static void h1_runner(void *arg) {
    (void)arg;

    sw_module_t *mod = sw_module_register("h1_mod", h1_worker_v1);
    int registered = (mod != NULL && mod->version == 1);

    sw_module_spawn("h1_mod", NULL);
    usleep(100000);

    h1_passed = registered && h1_worker_ran;
    printf("  Registered: %d, Worker ran: %d\n", registered, h1_worker_ran);
}

static void test_module_register(void) {
    printf("\n=== H1: Module register + spawn ===\n");
    h1_worker_ran = 0;
    h1_passed = 0;
    sw_spawn(h1_runner, NULL);
    usleep(400000);
    TEST_CHECK("module_register", h1_passed);
}

/* =========================================================================
 * H2: Module upgrade notification
 * ========================================================================= */

static volatile int h2_code_change_received = 0;
static volatile uint32_t h2_new_version = 0;

static void h2_worker(void *arg) {
    (void)arg;

    /* Wait for code change notification */
    uint64_t tag = 0;
    void *msg = sw_receive_any(3000, &tag);
    if (tag == SW_TAG_CODE_CHANGE) {
        sw_code_change_t *cc = (sw_code_change_t *)msg;
        h2_code_change_received = 1;
        h2_new_version = cc->new_version;
        free(cc);
    } else if (msg) free(msg);
}

static void h2_worker_v2(void *arg) {
    (void)arg;
    /* New version — just exists */
    sw_receive(500);
}

static volatile int h2_passed = 0;

static void h2_runner(void *arg) {
    (void)arg;

    sw_module_register("h2_mod", h2_worker);
    sw_module_spawn("h2_mod", NULL);
    usleep(50000); /* Let worker start */

    /* Upgrade module */
    sw_module_upgrade("h2_mod", h2_worker_v2);
    usleep(200000); /* Let notification arrive */

    uint32_t version = sw_module_version("h2_mod");
    h2_passed = h2_code_change_received && h2_new_version == 2 && version == 2;
    printf("  Code change received: %d, New version: %u, Current: %u\n",
           h2_code_change_received, h2_new_version, version);
}

static void test_module_upgrade(void) {
    printf("\n=== H2: Module upgrade notification ===\n");
    h2_code_change_received = 0;
    h2_new_version = 0;
    h2_passed = 0;
    sw_spawn(h2_runner, NULL);
    usleep(600000);
    TEST_CHECK("module_upgrade", h2_passed);
}

/* =========================================================================
 * H3: Module rollback
 * ========================================================================= */

static volatile int h3_passed = 0;

static void h3_v1(void *arg) { (void)arg; sw_receive(500); }
static void h3_v2(void *arg) { (void)arg; sw_receive(500); }

static void h3_runner(void *arg) {
    (void)arg;

    sw_module_register("h3_mod", h3_v1);
    sw_module_upgrade("h3_mod", h3_v2);

    sw_module_t *mod = sw_module_find("h3_mod");
    int is_v2 = (mod && mod->current_func == h3_v2 && mod->version == 2);

    sw_module_rollback("h3_mod");
    mod = sw_module_find("h3_mod");
    int is_v1_again = (mod && mod->current_func == h3_v1 && mod->version == 3);

    h3_passed = is_v2 && is_v1_again;
    printf("  After upgrade: v2=%d, After rollback: v1_again=%d (version=%u)\n",
           is_v2, is_v1_again, mod ? mod->version : 0);
}

static void test_module_rollback(void) {
    printf("\n=== H3: Module rollback ===\n");
    h3_passed = 0;
    sw_spawn(h3_runner, NULL);
    usleep(300000);
    TEST_CHECK("module_rollback", h3_passed);
}

/* =========================================================================
 * H4: Multiple processes get upgrade notification
 * ========================================================================= */

static volatile int h4_notifications = 0;

static void h4_worker(void *arg) {
    (void)arg;
    uint64_t tag = 0;
    void *msg = sw_receive_any(3000, &tag);
    if (tag == SW_TAG_CODE_CHANGE) {
        __sync_fetch_and_add(&h4_notifications, 1);
        free(msg);
    } else if (msg) free(msg);
}

static void h4_worker_v2(void *arg) { (void)arg; sw_receive(500); }

static volatile int h4_passed = 0;

static void h4_runner(void *arg) {
    (void)arg;

    sw_module_register("h4_mod", h4_worker);

    /* Spawn 2 workers (3 schedulers available after runner) */
    sw_module_spawn_link("h4_mod", NULL);
    sw_module_spawn_link("h4_mod", NULL);
    usleep(50000);

    sw_module_upgrade("h4_mod", h4_worker_v2);
    usleep(200000);

    h4_passed = (h4_notifications == 2);
    printf("  Notifications: %d (expected 2)\n", h4_notifications);
}

static void test_multi_upgrade(void) {
    printf("\n=== H4: Multiple processes upgrade ===\n");
    h4_notifications = 0;
    h4_passed = 0;
    sw_spawn(h4_runner, NULL);
    usleep(600000);
    TEST_CHECK("multi_upgrade", h4_passed);
}

/* =========================================================================
 * H5: Dead process cleanup — upgrade doesn't notify dead processes
 * ========================================================================= */

static volatile int h5_notifications = 0;

static void h5_mortal(void *arg) {
    (void)arg;
    /* Exit immediately — should be cleaned up from module tracking */
}

static void h5_stayer(void *arg) {
    (void)arg;
    uint64_t tag = 0;
    void *msg = sw_receive_any(3000, &tag);
    if (tag == SW_TAG_CODE_CHANGE) {
        __sync_fetch_and_add(&h5_notifications, 1);
        free(msg);
    } else if (msg) free(msg);
}

static void h5_v2(void *arg) { (void)arg; }

static volatile int h5_passed = 0;

static void h5_runner(void *arg) {
    (void)arg;

    sw_module_register("h5_mod", h5_mortal);

    /* Spawn a mortal (exits immediately) */
    sw_module_spawn_link("h5_mod", NULL);
    usleep(100000); /* Let it die */

    /* Now register stayer under same module with an upgrade */
    /* Actually, module_spawn uses current_func. Override current to stayer first */
    sw_module_upgrade("h5_mod", h5_stayer);
    /* The mortal already died, so the upgrade notification should only reach... nobody
     * (the mortal is dead and cleaned up). No notifications expected. */

    /* Spawn a stayer under the upgraded module */
    sw_module_spawn_link("h5_mod", NULL);
    usleep(50000);

    /* Upgrade again — only the stayer should get it */
    sw_module_upgrade("h5_mod", h5_v2);
    usleep(200000);

    h5_passed = (h5_notifications == 1);
    printf("  Notifications: %d (expected 1 — mortal dead, stayer alive)\n", h5_notifications);
}

static void test_dead_cleanup(void) {
    printf("\n=== H5: Dead process cleanup ===\n");
    h5_notifications = 0;
    h5_passed = 0;
    sw_spawn(h5_runner, NULL);
    usleep(800000);
    TEST_CHECK("dead_cleanup", h5_passed);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("============================================\n");
    printf("  SwarmRT Phase 7: Hot Code Reload\n");
    printf("============================================\n");

    if (sw_init("phase7-test", 4) != 0) {
        fprintf(stderr, "Failed to initialize SwarmRT\n");
        return 1;
    }

    test_module_register();
    test_module_upgrade();
    test_module_rollback();
    test_multi_upgrade();
    test_dead_cleanup();

    sw_stats(0);
    sw_shutdown(0);

    printf("\n============================================\n");
    printf("  Results: %d passed, %d failed (of %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
