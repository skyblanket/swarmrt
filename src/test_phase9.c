/*
 * SwarmRT Phase 9: Node & Distribution Tests
 *
 * Tests run within a single OS process — two SwarmRT node instances
 * communicate over TCP loopback. Since we can only have one sw_init,
 * we test node start/connect/send at the library level.
 *
 * 4 tests: node start, connect, send message, peer list.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include "swarmrt_node.h"
#include "swarmrt_otp.h"
#include "swarmrt_ets.h"
#include "swarmrt_phase5.h"
#include "swarmrt_hotload.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_PASS(name) do { tests_passed++; printf("  [PASS] %s\n", name); } while(0)
#define TEST_FAIL(name) do { tests_failed++; printf("  [FAIL] %s\n", name); } while(0)
#define TEST_CHECK(name, cond) do { if (cond) TEST_PASS(name); else TEST_FAIL(name); } while(0)

/* =========================================================================
 * N1: Node start
 * ========================================================================= */

static void test_node_start(void) {
    printf("\n=== N1: Node start ===\n");

    int ok = sw_node_start("alpha@localhost", 19200);
    const char *name = sw_node_name();

    int passed = (ok == 0 && name && strcmp(name, "alpha@localhost") == 0);
    printf("  Started: %d, Name: %s\n", ok == 0, name ? name : "NULL");
    TEST_CHECK("node_start", passed);
}

/* =========================================================================
 * N2: Node connect (connect to self as a loopback test)
 * ========================================================================= */

static void test_node_connect(void) {
    printf("\n=== N2: Node connect ===\n");

    /* Connect to our own listener as a loopback test */
    usleep(100000); /* Let listener start */
    int ok = sw_node_connect("beta@localhost", "127.0.0.1", 19200);

    int connected = sw_node_is_connected("beta@localhost");
    int passed = (ok == 0 && connected);
    printf("  Connected: %d, Is connected: %d\n", ok == 0, connected);
    TEST_CHECK("node_connect", passed);
}

/* =========================================================================
 * N3: Send message to named process across "nodes"
 * ========================================================================= */

static volatile int n3_received = 0;

static void n3_receiver(void *arg) {
    (void)arg;
    sw_register("n3_target", sw_self());

    uint64_t tag = 0;
    void *msg = sw_receive_any(3000, &tag);
    if (tag == SW_TAG_REMOTE_MSG || msg != NULL) {
        n3_received = 1;
    }
    if (msg) free(msg);
}

static volatile int n3_passed = 0;

static void n3_sender(void *arg) {
    (void)arg;
    usleep(100000); /* Let receiver register */

    /* Send to "n3_target" on "beta@localhost" (which is really us via loopback) */
    int ok = sw_node_send("beta@localhost", "n3_target", SW_TAG_CAST, "hello", 5);

    usleep(200000);
    n3_passed = (ok == 0 && n3_received);
    printf("  Send OK: %d, Received: %d\n", ok == 0, n3_received);
}

static void test_node_send(void) {
    printf("\n=== N3: Node send message ===\n");
    n3_received = 0;
    n3_passed = 0;
    sw_spawn(n3_receiver, NULL);
    sw_spawn(n3_sender, NULL);
    usleep(600000);
    TEST_CHECK("node_send", n3_passed);
}

/* =========================================================================
 * N4: Peer list
 * ========================================================================= */

static void test_peer_list(void) {
    printf("\n=== N4: Peer list ===\n");

    char names[8][SW_NODE_NAME_MAX];
    int count = sw_node_peers(names, 8);

    int passed = (count >= 1 && strcmp(names[0], "beta@localhost") == 0);
    printf("  Peers: %d", count);
    for (int i = 0; i < count; i++) {
        printf(" [%s]", names[i]);
    }
    printf("\n");
    TEST_CHECK("peer_list", passed);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("============================================\n");
    printf("  SwarmRT Phase 9: Node & Distribution\n");
    printf("============================================\n");

    if (sw_init("phase9-test", 4) != 0) {
        fprintf(stderr, "Failed to initialize SwarmRT\n");
        return 1;
    }

    if (sw_io_init() != 0) {
        fprintf(stderr, "Failed to initialize IO system\n");
        return 1;
    }

    test_node_start();
    test_node_connect();
    test_node_send();
    test_peer_list();

    sw_node_stop();
    sw_io_shutdown();
    sw_stats(0);
    sw_shutdown(0);

    printf("\n============================================\n");
    printf("  Results: %d passed, %d failed (of %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
