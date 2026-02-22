/*
 * SwarmRT Phase 6: IO System Tests
 *
 * 6 tests: TCP listen/accept/send/recv, port close, echo server.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include "swarmrt_io.h"
#include "swarmrt_otp.h"
#include "swarmrt_ets.h"
#include "swarmrt_phase5.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_PASS(name) do { tests_passed++; printf("  [PASS] %s\n", name); } while(0)
#define TEST_FAIL(name) do { tests_failed++; printf("  [FAIL] %s\n", name); } while(0)
#define TEST_CHECK(name, cond) do { if (cond) TEST_PASS(name); else TEST_FAIL(name); } while(0)

/* Port for tests — use high port to avoid conflicts */
static uint16_t test_port_base = 19100;

/* =========================================================================
 * I1: TCP listen + accept
 * ========================================================================= */

static volatile int i1_accepted = 0;

static void i1_server(void *arg) {
    (void)arg;
    sw_port_t *listener = sw_tcp_listen("127.0.0.1", test_port_base);
    if (!listener) { printf("  listen failed\n"); return; }

    /* Wait for accept */
    uint64_t tag = 0;
    void *msg = sw_receive_any(3000, &tag);
    if (tag == SW_TAG_PORT_ACCEPT) {
        sw_port_accept_t *acc = (sw_port_accept_t *)msg;
        i1_accepted = (acc->conn != NULL);
        sw_port_close(acc->conn);
        free(msg);
    } else if (msg) free(msg);

    sw_port_close(listener);
}

static void i1_client(void *arg) {
    (void)arg;
    usleep(50000); /* Let server start */
    sw_port_t *conn = sw_tcp_connect("127.0.0.1", test_port_base);
    if (conn) sw_port_close(conn);
}

static void test_tcp_listen_accept(void) {
    printf("\n=== I1: TCP listen + accept ===\n");
    i1_accepted = 0;
    sw_spawn(i1_server, NULL);
    sw_spawn(i1_client, NULL);
    usleep(500000);
    TEST_CHECK("tcp_listen_accept", i1_accepted);
}

/* =========================================================================
 * I2: TCP send + receive data
 * ========================================================================= */

static volatile int i2_data_ok = 0;

static void i2_server(void *arg) {
    uint16_t port = test_port_base + 1;
    (void)arg;
    sw_port_t *listener = sw_tcp_listen("127.0.0.1", port);
    if (!listener) return;

    /* Accept */
    uint64_t tag = 0;
    void *msg = sw_receive_any(3000, &tag);
    if (tag != SW_TAG_PORT_ACCEPT) { if (msg) free(msg); sw_port_close(listener); return; }

    sw_port_accept_t *acc = (sw_port_accept_t *)msg;
    sw_port_t *conn = acc->conn;
    free(msg);

    /* Read data */
    msg = sw_receive_any(3000, &tag);
    if (tag == SW_TAG_PORT_DATA) {
        sw_port_data_t *data = (sw_port_data_t *)msg;
        i2_data_ok = (data->len == 5 && memcmp(data->data, "hello", 5) == 0);
        printf("  Received: '%.*s' (%u bytes)\n", data->len, data->data, data->len);
        free(data->data);
        free(data);
    } else if (msg) free(msg);

    sw_port_close(conn);
    sw_port_close(listener);
}

static void i2_client(void *arg) {
    uint16_t port = test_port_base + 1;
    (void)arg;
    usleep(50000);
    sw_port_t *conn = sw_tcp_connect("127.0.0.1", port);
    if (!conn) return;

    sw_tcp_send(conn, "hello", 5);
    usleep(100000);
    sw_port_close(conn);
}

static void test_tcp_send_recv(void) {
    printf("\n=== I2: TCP send + receive ===\n");
    i2_data_ok = 0;
    sw_spawn(i2_server, NULL);
    sw_spawn(i2_client, NULL);
    usleep(800000);
    TEST_CHECK("tcp_send_recv", i2_data_ok);
}

/* =========================================================================
 * I3: TCP echo server (round-trip)
 * ========================================================================= */

static volatile int i3_echo_ok = 0;

static void i3_echo_server(void *arg) {
    uint16_t port = test_port_base + 2;
    (void)arg;
    sw_port_t *listener = sw_tcp_listen("127.0.0.1", port);
    if (!listener) return;

    /* Accept */
    uint64_t tag = 0;
    void *msg = sw_receive_any(3000, &tag);
    if (tag != SW_TAG_PORT_ACCEPT) { if (msg) free(msg); sw_port_close(listener); return; }

    sw_port_accept_t *acc = (sw_port_accept_t *)msg;
    sw_port_t *conn = acc->conn;
    free(msg);

    /* Echo loop — handle up to 10 messages */
    for (int i = 0; i < 10; i++) {
        msg = sw_receive_any(2000, &tag);
        if (tag == SW_TAG_PORT_DATA) {
            sw_port_data_t *data = (sw_port_data_t *)msg;
            sw_tcp_send(conn, data->data, data->len);
            free(data->data);
            free(data);
        } else if (tag == SW_TAG_PORT_CLOSED) {
            if (msg) free(msg);
            break;
        } else {
            if (msg) free(msg);
            break;
        }
    }

    sw_port_close(conn);
    sw_port_close(listener);
}

static void i3_client(void *arg) {
    uint16_t port = test_port_base + 2;
    (void)arg;
    usleep(50000);
    sw_port_t *conn = sw_tcp_connect("127.0.0.1", port);
    if (!conn) return;

    sw_tcp_send(conn, "ping!", 5);

    /* Read echo */
    uint64_t tag = 0;
    void *msg = sw_receive_any(3000, &tag);
    if (tag == SW_TAG_PORT_DATA) {
        sw_port_data_t *data = (sw_port_data_t *)msg;
        i3_echo_ok = (data->len == 5 && memcmp(data->data, "ping!", 5) == 0);
        printf("  Echo: '%.*s' (%u bytes)\n", data->len, data->data, data->len);
        free(data->data);
        free(data);
    } else if (msg) free(msg);

    sw_port_close(conn);
}

static void test_tcp_echo(void) {
    printf("\n=== I3: TCP echo server ===\n");
    i3_echo_ok = 0;
    sw_spawn(i3_echo_server, NULL);
    sw_spawn(i3_client, NULL);
    usleep(800000);
    TEST_CHECK("tcp_echo", i3_echo_ok);
}

/* =========================================================================
 * I4: Port close notification
 * ========================================================================= */

static volatile int i4_close_received = 0;

static void i4_server(void *arg) {
    uint16_t port = test_port_base + 3;
    (void)arg;
    sw_port_t *listener = sw_tcp_listen("127.0.0.1", port);
    if (!listener) return;

    uint64_t tag = 0;
    void *msg = sw_receive_any(3000, &tag);
    if (tag != SW_TAG_PORT_ACCEPT) { if (msg) free(msg); sw_port_close(listener); return; }

    sw_port_accept_t *acc = (sw_port_accept_t *)msg;
    sw_port_t *conn = acc->conn;
    free(msg);

    /* Wait for client to close */
    msg = sw_receive_any(3000, &tag);
    if (tag == SW_TAG_PORT_CLOSED) {
        i4_close_received = 1;
    }
    if (msg) free(msg);

    sw_port_close(conn);
    sw_port_close(listener);
}

static void i4_client(void *arg) {
    uint16_t port = test_port_base + 3;
    (void)arg;
    usleep(50000);
    sw_port_t *conn = sw_tcp_connect("127.0.0.1", port);
    if (!conn) return;

    usleep(50000);
    sw_port_close(conn);
}

static void test_port_close_notify(void) {
    printf("\n=== I4: Port close notification ===\n");
    i4_close_received = 0;
    sw_spawn(i4_server, NULL);
    sw_spawn(i4_client, NULL);
    usleep(500000);
    TEST_CHECK("port_close_notify", i4_close_received);
}

/* =========================================================================
 * I5: Controlling process transfer
 * ========================================================================= */

static volatile int i5_passed = 0;

static void i5_new_owner(void *arg) {
    (void)arg;
    uint64_t tag = 0;
    void *msg = sw_receive_any(3000, &tag);

    /* Should receive data after ownership transfer */
    if (tag == SW_TAG_PORT_DATA) {
        sw_port_data_t *data = (sw_port_data_t *)msg;
        i5_passed = (data->len > 0);
        free(data->data);
        free(data);
    } else if (msg) free(msg);
}

static void i5_server(void *arg) {
    uint16_t port = test_port_base + 4;
    (void)arg;
    sw_port_t *listener = sw_tcp_listen("127.0.0.1", port);
    if (!listener) return;

    uint64_t tag = 0;
    void *msg = sw_receive_any(3000, &tag);
    if (tag != SW_TAG_PORT_ACCEPT) { if (msg) free(msg); sw_port_close(listener); return; }

    sw_port_accept_t *acc = (sw_port_accept_t *)msg;
    sw_port_t *conn = acc->conn;
    free(msg);

    /* Transfer ownership */
    sw_process_t *new_owner = sw_spawn_link(i5_new_owner, NULL);
    usleep(10000);
    sw_port_controlling_process(conn, new_owner);

    /* Wait for new owner to finish */
    usleep(500000);

    sw_port_close(conn);
    sw_port_close(listener);
}

static void i5_client(void *arg) {
    uint16_t port = test_port_base + 4;
    (void)arg;
    usleep(100000);
    sw_port_t *conn = sw_tcp_connect("127.0.0.1", port);
    if (!conn) return;

    usleep(200000); /* Wait for server to accept + transfer ownership */
    sw_tcp_send(conn, "transferred", 11);
    usleep(200000);
    sw_port_close(conn);
}

static void test_controlling_process(void) {
    printf("\n=== I5: Controlling process transfer ===\n");
    i5_passed = 0;
    sw_spawn(i5_server, NULL);
    sw_spawn(i5_client, NULL);
    usleep(1000000);
    TEST_CHECK("controlling_process", i5_passed);
}

/* =========================================================================
 * I6: Multiple concurrent connections
 * ========================================================================= */

static volatile int i6_total_received = 0;

static void i6_server(void *arg) {
    uint16_t port = test_port_base + 5;
    (void)arg;
    sw_port_t *listener = sw_tcp_listen("127.0.0.1", port);
    if (!listener) return;

    /* Accept 2 connections, read from each (4 events: 2 accepts + 2 data) */
    for (int i = 0; i < 4; i++) {
        uint64_t tag = 0;
        void *msg = sw_receive_any(3000, &tag);
        if (tag == SW_TAG_PORT_ACCEPT) {
            free(msg);
        } else if (tag == SW_TAG_PORT_DATA) {
            sw_port_data_t *data = (sw_port_data_t *)msg;
            __sync_fetch_and_add(&i6_total_received, data->len);
            free(data->data);
            free(data);
        } else if (msg) free(msg);
    }

    sw_port_close(listener);
}

static void i6_connector(void *arg) {
    uint16_t port = test_port_base + 5;
    (void)arg;
    usleep(50000);
    sw_port_t *conn = sw_tcp_connect("127.0.0.1", port);
    if (!conn) return;
    sw_tcp_send(conn, "data", 4);
    usleep(200000);
    sw_port_close(conn);
}

static void test_multi_connections(void) {
    printf("\n=== I6: Multiple concurrent connections ===\n");
    i6_total_received = 0;
    sw_spawn(i6_server, NULL);
    /* Spawn connectors from main thread — round-robin avoids server's scheduler */
    sw_spawn(i6_connector, NULL);
    sw_spawn(i6_connector, NULL);
    usleep(1000000);
    printf("  Total bytes received: %d (expected 8)\n", i6_total_received);
    TEST_CHECK("multi_connections", i6_total_received == 8);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("============================================\n");
    printf("  SwarmRT Phase 6: IO System (kqueue + TCP)\n");
    printf("============================================\n");

    if (sw_init("phase6-test", 4) != 0) {
        fprintf(stderr, "Failed to initialize SwarmRT\n");
        return 1;
    }

    if (sw_io_init() != 0) {
        fprintf(stderr, "Failed to initialize IO system\n");
        return 1;
    }

    test_tcp_listen_accept();
    test_tcp_send_recv();
    test_tcp_echo();
    test_port_close_notify();
    test_controlling_process();
    test_multi_connections();

    sw_io_shutdown();
    sw_stats(0);
    sw_shutdown(0);

    printf("\n============================================\n");
    printf("  Results: %d passed, %d failed (of %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
