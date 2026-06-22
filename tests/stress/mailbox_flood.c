/*
 * Mailbox depth-cap gate (SW_MAILBOX_MAX) — bidirectional.
 *
 * Capped mode   (run with SW_MAILBOX_MAX=1000, argv[1]=capped):
 *   T1  Flood a parked receiver with 200k messages (100k RAW sw_send_tagged
 *       + 100k VALUE sw_send_value). Exactly cap (1000) must be admitted,
 *       the rest dropped (sw_mailbox_dropped() == 199000 — exact accounting,
 *       single sender), and the receiver must survive and drain cleanly.
 *       Under ASAN this also proves the drop path frees RAW payloads and
 *       VALUE regions (no leak / UAF / double-free).
 *   T2  sw_call into a flooded, non-receiving server with timeout_ms=200
 *       must return NULL promptly (dropped CALL surfaces as timeout, never
 *       a hang). The server then exits with a full mailbox (process_destroy
 *       reclaims the queue + resets the counter).
 *   T3  Under a FULL mailbox: (a) a receive timeout still fires (wake-up
 *       timer path), and (b) a real timer MESSAGE (sw_send_after) is still
 *       delivered — timer fires are exempt from the cap. Without the
 *       exemption (b) fails: the fire would be dropped.
 *   T4  An EXIT signal to a flooded trap_exit process is still delivered —
 *       deliver_signal is exempt, so supervision survives mailbox floods.
 *
 * Uncapped mode (run with SW_MAILBOX_MAX=0, argv[1]=uncapped):
 *   T1 only: ALL 200k messages must arrive and sw_mailbox_dropped() must be
 *   0 — proving the cap never fires when disabled AND that the accounting
 *   has no false drops. This run doubles as the see-it-fail control: the
 *   capped-mode assertions (drops > 0, admitted == cap) fail against a
 *   runtime without the cap, i.e. the gate is bidirectional.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include "swarmrt_native.h"
#include "swarmrt_otp.h"
#include "swarmrt_lang.h"

#define CAP        1000      /* must match SW_MAILBOX_MAX in the capped run */
#define N_RAW      100000
#define N_VAL      100000
#define N_TOTAL    (N_RAW + N_VAL)
#define OVERFILL   1500      /* self-fill count for T2/T3/T4 (> CAP) */

static int g_capped = 0;
static int g_failures = 0;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int wait_flag(_Atomic int *flag, int timeout_ms) {
    long deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        if (atomic_load(flag)) return 1;
        usleep(10000);
    }
    return atomic_load(flag);
}

static void check(int ok, const char *what) {
    printf("  %-64s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) g_failures++;
}

/* =========================================================================
 * T1: flood a parked receiver — cap admits exactly CAP, drops the rest
 * ========================================================================= */

static sw_process_t *g_recv;
static _Atomic int g_flood_done;
static _Atomic long g_drained;
static _Atomic int g_recv_done;

static void receiver_entry(void *arg) {
    (void)arg;
    /* Stay OUT of receive while the sender floods (parked, not waiting). */
    while (!atomic_load(&g_flood_done)) sw_yield();
    long n = 0;
    while (1) {
        uint64_t tag = 0;
        void *p = sw_receive_any(0, &tag);
        if (!p) break;
        /* tag 42 = RAW malloc'd uint64; tag 7 = VALUE materialized to a
         * single global-heap sw_val_t int — both are one free(). */
        free(p);
        n++;
    }
    atomic_store(&g_drained, n);
    atomic_store(&g_recv_done, 1);
}

static void sender_entry(void *arg) {
    (void)arg;
    for (int i = 0; i < N_RAW; i++) {
        uint64_t *p = (uint64_t *)malloc(sizeof(*p));
        *p = (uint64_t)i;
        sw_send_tagged(g_recv, 42, p);
        if ((i & 4095) == 0) sw_yield();
    }
    for (int i = 0; i < N_VAL; i++) {
        sw_send_value(g_recv, 7, sw_val_int(i));
        if ((i & 4095) == 0) sw_yield();
    }
    atomic_store(&g_flood_done, 1);
}

static void test_flood(void) {
    printf("\n=== T1: %d-message flood into a parked receiver (%s) ===\n",
           N_TOTAL, g_capped ? "cap=1000" : "cap disabled");
    g_recv = sw_spawn(receiver_entry, NULL);
    sw_spawn(sender_entry, NULL);

    int done = wait_flag(&g_recv_done, 120000);
    check(done, "receiver survived the flood and finished draining");
    if (!done) return;

    long drained = atomic_load(&g_drained);
    unsigned long long dropped = (unsigned long long)sw_mailbox_dropped();
    printf("  drained=%ld dropped=%llu\n", drained, dropped);
    if (g_capped) {
        check(drained == CAP,
              "exactly cap messages admitted (single sender => exact)");
        check(dropped == (unsigned long long)(N_TOTAL - CAP),
              "dropped counter == flood - cap (exact accounting)");
    } else {
        check(drained == N_TOTAL, "ALL messages arrived with cap disabled");
        check(dropped == 0, "zero drops with cap disabled (no false drops)");
    }
}

/* =========================================================================
 * T2: sw_call into a flooded server -> timeout (NULL), never a hang
 * ========================================================================= */

static sw_process_t *g_srv;
static _Atomic int g_srv_stop;
static _Atomic int g_t2_done;
static _Atomic int g_t2_pass;

static void server_entry(void *arg) {
    (void)arg;
    /* Never receives — exits with a full mailbox so process_destroy has to
     * reclaim the queued payloads and reset the depth counter. */
    while (!atomic_load(&g_srv_stop)) sw_yield();
}

static void caller_entry(void *arg) {
    (void)arg;
    for (int i = 0; i < OVERFILL; i++) {
        uint64_t *p = (uint64_t *)malloc(sizeof(*p));
        *p = (uint64_t)i;
        sw_send_tagged(g_srv, 42, p);
    }
    long t0 = now_ms();
    void *r = sw_call_proc(g_srv, (void *)"ping", 200);
    long elapsed = now_ms() - t0;
    atomic_store(&g_t2_pass, r == NULL && elapsed < 5000);
    atomic_store(&g_srv_stop, 1);
    atomic_store(&g_t2_done, 1);
}

static void test_call_timeout(void) {
    printf("\n=== T2: sw_call into a flooded server (timeout, not hang) ===\n");
    g_srv = sw_spawn(server_entry, NULL);
    sw_spawn(caller_entry, NULL);
    int done = wait_flag(&g_t2_done, 30000);
    check(done && atomic_load(&g_t2_pass),
          "dropped CALL surfaced as NULL within timeout (no deadlock)");
}

/* =========================================================================
 * T3: timer semantics survive a FULL mailbox (timer-fire cap exemption)
 * ========================================================================= */

static _Atomic int g_t3_done;
static _Atomic int g_t3_pass;

static void t3_entry(void *arg) {
    (void)arg;
    sw_process_t *self = sw_self();
    for (int i = 0; i < OVERFILL; i++) {
        uint64_t *p = (uint64_t *)malloc(sizeof(*p));
        *p = (uint64_t)i;
        sw_send_tagged(self, 42, p);
    }
    int pass = 1;

    /* (a) receive timeout fires under a full mailbox (wake-up timer path) */
    long t0 = now_ms();
    void *r = sw_receive_tagged(77, 150);
    if (r != NULL || now_ms() - t0 > 5000) pass = 0;

    /* (b) a timer MESSAGE is delivered past the cap (exempt producer) */
    uint64_t *tp = (uint64_t *)malloc(sizeof(*tp));
    *tp = 0xBEEF;
    sw_send_after(50, self, 99, tp);
    void *m = sw_receive_tagged(99, 3000);
    if (!m) pass = 0;
    else free(m);

    /* drain + free the self-flood */
    while ((m = sw_receive(0)) != NULL) free(m);

    atomic_store(&g_t3_pass, pass);
    atomic_store(&g_t3_done, 1);
}

static void test_timer_exemption(void) {
    printf("\n=== T3: receive-after + send_after under a full mailbox ===\n");
    sw_spawn(t3_entry, NULL);
    int done = wait_flag(&g_t3_done, 30000);
    check(done && atomic_load(&g_t3_pass),
          "timeout fired AND timer message delivered past the cap");
}

/* =========================================================================
 * T4: EXIT signal to a flooded trap_exit process is still delivered
 * ========================================================================= */

static sw_process_t *g_t4a;
static _Atomic int g_t4a_full;
static _Atomic int g_t4_done;
static _Atomic int g_t4_pass;

static void t4a_entry(void *arg) {
    (void)arg;
    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);
    sw_process_t *self = sw_self();
    for (int i = 0; i < OVERFILL; i++) {
        uint64_t *p = (uint64_t *)malloc(sizeof(*p));
        *p = (uint64_t)i;
        sw_send_tagged(self, 42, p);
    }
    atomic_store(&g_t4a_full, 1);
    void *sigp = sw_receive_tagged(SW_TAG_EXIT, 8000);
    int pass = 0;
    if (sigp) {
        sw_signal_t *s = (sw_signal_t *)sigp;
        pass = (s->reason == 5);
        free(s->reason_str);
        free(s);
    }
    void *m;
    while ((m = sw_receive(0)) != NULL) free(m);
    atomic_store(&g_t4_pass, pass);
    atomic_store(&g_t4_done, 1);
}

static void t4b_entry(void *arg) {
    (void)arg;
    while (!atomic_load(&g_t4a_full)) sw_yield();
    sw_link(g_t4a);
    /* Panic banner on stderr is expected output here. */
    sw_process_panic(sw_self(), 5, "mailbox-flood EXIT-exemption test");
}

static void test_exit_exemption(void) {
    printf("\n=== T4: EXIT to a flooded trap_exit process ===\n");
    g_t4a = sw_spawn(t4a_entry, NULL);
    sw_spawn(t4b_entry, NULL);
    int done = wait_flag(&g_t4_done, 30000);
    check(done && atomic_load(&g_t4_pass),
          "EXIT (reason 5) delivered past a full mailbox (trap_exit)");
}

/* ========================================================================= */

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2 ||
        (strcmp(argv[1], "capped") != 0 && strcmp(argv[1], "uncapped") != 0)) {
        fprintf(stderr, "usage: %s capped|uncapped  (with SW_MAILBOX_MAX set "
                        "to 1000 / 0 respectively)\n", argv[0]);
        return 2;
    }
    g_capped = strcmp(argv[1], "capped") == 0;

    /* Refuse to run with a mismatched env — a silent mis-run would turn the
     * exact-accounting assertions into noise. */
    const char *env = getenv("SW_MAILBOX_MAX");
    const char *want = g_capped ? "1000" : "0";
    if (!env || strcmp(env, want) != 0) {
        fprintf(stderr, "mailbox_flood: %s mode requires SW_MAILBOX_MAX=%s "
                        "(got %s)\n", argv[1], want, env ? env : "(unset)");
        return 2;
    }

    printf("==============================================\n");
    printf("  SwarmRT mailbox depth-cap gate (%s)\n", argv[1]);
    printf("==============================================\n");

    if (sw_init("mailbox-flood", 4) != 0) {
        fprintf(stderr, "sw_init failed\n");
        return 1;
    }
    usleep(50000); /* scheduler warm-up */

    test_flood();
    if (g_capped) {
        test_call_timeout();
        test_timer_exemption();
        test_exit_exemption();
    }

    printf("\n%s: %s\n", argv[1], g_failures ? "FAIL" : "PASS");
    sw_shutdown(0);
    return g_failures ? 1 : 0;
}
