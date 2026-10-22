/*
 * Stackless processes: the suspend protocol, driven by hand-written frames
 * shaped like the code Phase 2c's codegen will emit
 * (docs/design/PROCESS_WEIGHT.md §3.3–3.5).
 *
 *   1. A receive loop that parks with no carrier, inside a caller that is
 *      itself suspended mid-call: the callee's value must reach the caller
 *      when the chain resumes.
 *   2. A yield every few messages (SW_WAIT_YIELD).
 *   3. 10,000 parked stackless processes hold no stack mappings.
 *   4. A process killed while parked is torn down, frames included.
 */
#include "swarmrt_native.h"
#include "swarmrt_lang.h"
#include "swarmrt_varena.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) printf("  PASS "); else { printf("  FAIL "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- sumloop(n, acc): receive n ints, yielding every 4, return the sum ---- */
typedef struct { sw_frame_t h; int64_t n, acc, seen; } sum_F;
static const sw_fn_meta_t sum_meta = { "T", "sumloop", 1 };
static sw_val_t *sumloop_b(int64_t n, int64_t acc, sw_frame_t *rs);
static sw_val_t *sumloop_rs(sw_frame_t *f) { return sumloop_b(0, 0, f); }

static sw_val_t *sumloop_b(int64_t n, int64_t acc, sw_frame_t *rs) {
    int64_t seen = 0;
    int pt = 0;
    if (rs) {
        sum_F *f = (sum_F *)rs;
        n = f->n; acc = f->acc; seen = f->seen;
        switch (rs->pt) { case 1: goto rs1; case 2: goto rs2; default: abort(); }
    }
    while (n > 0) {
rs1:;
        sw_msg_t *m = sw_mailbox_peek();
        if (!m) {
            if (sw_suspend_ok()) { sw_suspend_recv(UINT64_MAX); pt = 1; goto capture; }
            sw_mailbox_wait_new(UINT64_MAX);
            continue;
        }
        sw_mailbox_remove_msg(m);
        { sw_value_arena_t *rv = sw_self_varena();
          if (m->pkind == SW_PK_VALUE && m->region && rv) { sw_varena_adopt(rv, m->region); m->region = NULL; } }
        sw_val_t *v = (sw_val_t *)m->payload;
        acc += v ? v->v.i : 0;
        sw_msg_release(m);
        n--;
        if (++seen % 4 == 0) {
            if (sw_suspend_ok()) { sw_suspend_yield(); pt = 2; goto capture; }
            sw_yield();
        }
rs2:;
    }
    return sw_val_int(acc);
capture: {
    sum_F *f = rs ? (sum_F *)rs : (sum_F *)sw_frame_new(sizeof(sum_F), sumloop_rs, &sum_meta);
    f->h.pt = pt; f->n = n; f->acc = acc; f->seen = seen;
    sw_frame_link(&f->h, rs != NULL);
    return SW_SUSPEND;
  }
}

/* ---- outer(): base + sumloop(N, 1000), suspended across the call ---- */
static _Atomic long g_result = -1;
static _Atomic int g_done = 0;
static int64_t g_n = 0;
typedef struct { sw_frame_t h; int64_t base; } out_F;
static const sw_fn_meta_t out_meta = { "T", "outer", 2 };
static sw_val_t *outer_b(sw_frame_t *rs);
static sw_val_t *outer_rs(sw_frame_t *f) { return outer_b(f); }

static sw_val_t *outer_b(sw_frame_t *rs) {
    int64_t base = 5;
    sw_val_t *r;
    if (rs) {
        base = ((out_F *)rs)->base;
        switch (rs->pt) { case 1: r = sw_resume_value(); goto rs1; default: abort(); }
    }
    r = sumloop_b(g_n, 1000, NULL);
rs1:
    if (r == SW_SUSPEND) {
        out_F *f = rs ? (out_F *)rs : (out_F *)sw_frame_new(sizeof(out_F), outer_rs, &out_meta);
        f->h.pt = 1; f->base = base;
        sw_frame_link(&f->h, rs != NULL);
        return SW_SUSPEND;
    }
    atomic_store(&g_result, (long)(r->v.i + base));
    atomic_store(&g_done, 1);
    return r;
}

static sw_val_t *outer_entry(void *arg) { (void)arg; return outer_b(NULL); }

/* ---- parker(): park forever on an empty mailbox ---- */
typedef struct { sw_frame_t h; } park_F;
static const sw_fn_meta_t park_meta = { "T", "parker", 3 };
static sw_val_t *parker_b(sw_frame_t *rs);
static sw_val_t *parker_rs(sw_frame_t *f) { return parker_b(f); }
static _Atomic int g_parkers_started = 0;

static sw_val_t *parker_b(sw_frame_t *rs) {
    if (!rs) atomic_fetch_add(&g_parkers_started, 1);
    for (;;) {
        sw_msg_t *m = sw_mailbox_peek();
        if (m) { sw_mailbox_remove_msg(m); sw_msg_release(m); return sw_val_nil(); }
        if (!sw_suspend_ok()) { sw_mailbox_wait_new(UINT64_MAX); continue; }
        sw_suspend_recv(UINT64_MAX);
        park_F *f = rs ? (park_F *)rs : (park_F *)sw_frame_new(sizeof(park_F), parker_rs, &park_meta);
        f->h.pt = 1;
        sw_frame_link(&f->h, rs != NULL);
        return SW_SUSPEND;
    }
}
static sw_val_t *parker_entry(void *arg) { (void)arg; return parker_b(NULL); }

/* ---- crasher(): parks inside outer-style call, then panics on resume ---- */
typedef struct { sw_frame_t h; } crash_F;
static const sw_fn_meta_t crash_meta = { "T", "crasher", 4 };
static sw_val_t *crash_leaf(sw_frame_t *rs);
static sw_val_t *crash_leaf_rs(sw_frame_t *f) { return crash_leaf(f); }
static sw_val_t *crash_leaf(sw_frame_t *rs) {
    sw_msg_t *m = sw_mailbox_peek();
    if (m) {
        sw_mailbox_remove_msg(m); sw_msg_release(m);
        sw_process_panic(sw_self(), -1, "crash after resume");
        return sw_val_nil();
    }
    sw_suspend_recv(UINT64_MAX);
    crash_F *f = rs ? (crash_F *)rs : (crash_F *)sw_frame_new(sizeof(crash_F), crash_leaf_rs, &crash_meta);
    f->h.pt = 1;
    sw_frame_link(&f->h, rs != NULL);
    return SW_SUSPEND;
}
static sw_val_t *crash_mid(sw_frame_t *rs);
static sw_val_t *crash_mid_rs(sw_frame_t *f) { return crash_mid(f); }
static sw_val_t *crash_mid(sw_frame_t *rs) {
    sw_val_t *r = rs ? sw_resume_value() : crash_leaf(NULL);
    if (r == SW_SUSPEND) {
        crash_F *f = rs ? (crash_F *)rs : (crash_F *)sw_frame_new(sizeof(crash_F), crash_mid_rs, &crash_meta);
        f->h.pt = 1;
        sw_frame_link(&f->h, rs != NULL);
        return SW_SUSPEND;
    }
    return r;
}
static sw_val_t *crasher_entry(void *arg) { (void)arg; return crash_mid(NULL); }

static int count_maps(void) {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return -1;
    int n = 0, c;
    while ((c = fgetc(fp)) != EOF) if (c == '\n') n++;
    fclose(fp);
    return n;
}

static void wait_ms(int ms) { usleep((useconds_t)ms * 1000); }

int main(void) {
    printf("=== stackless frames ===\n");
    if (sw_init("frames", 4) != 0) { printf("sw_init failed\n"); return 1; }

    /* 1 + 2: a suspended call chain, fed from outside one message at a time */
    g_n = 50;
    sw_process_t *p = sw_spawn_v(outer_entry, NULL);
    CHECK(p != NULL, "spawned a value-returning process");
    int64_t want = 1000 + 5;
    for (int64_t i = 1; i <= g_n; i++) {
        sw_send_value(p, 0, sw_val_int(i));
        want += i;
        if (i % 10 == 0) wait_ms(5);   /* let it park between batches */
    }
    for (int i = 0; i < 2000 && !atomic_load(&g_done); i++) wait_ms(1);
    CHECK(atomic_load(&g_done), "the suspended chain finished");
    CHECK(atomic_load(&g_result) == want, "callee value reached the caller (%ld, want %lld)",
          atomic_load(&g_result), (long long)want);

    /* 3: parked stackless processes hold no stack mappings */
    int before = count_maps();
    enum { NPARK = 10000 };
    static sw_process_t *parked[NPARK];
    for (int i = 0; i < NPARK; i++) parked[i] = sw_spawn_v(parker_entry, NULL);
    for (int i = 0; i < 5000 && atomic_load(&g_parkers_started) < NPARK; i++) wait_ms(1);
    wait_ms(100);
    int after = count_maps();
    CHECK(atomic_load(&g_parkers_started) == NPARK, "%d parkers started", NPARK);
    CHECK(after - before < 200, "%d parked processes added %d mappings (a pinned stack costs 2 each)",
          NPARK, after - before);

    /* wake half of them with a message, kill the rest while parked */
    for (int i = 0; i < NPARK / 2; i++) sw_send_value(parked[i], 0, sw_val_nil());
    for (int i = NPARK / 2; i < NPARK; i++) sw_process_kill(parked[i], 1);
    wait_ms(300);
    int live = 0;
    for (int i = 0; i < NPARK; i++) {
        int s = atomic_load(&parked[i]->state);
        if (s == SW_PROC_WAITING || s == SW_PROC_RUNNABLE || s == SW_PROC_RUNNING) live++;
    }
    CHECK(live == 0, "woken and killed parkers all exited (%d still live)", live);
    CHECK(sw_frames_live() == 0, "their frames were freed (%ld still live)", sw_frames_live());

    /* 5: a panic in a resumed activation frees its frame chain (checked
     * here by count, and by LeakSanitizer under ASan with leaks on). */
    static sw_process_t *crashers[100];
    for (int i = 0; i < 100; i++) crashers[i] = sw_spawn_v(crasher_entry, NULL);
    wait_ms(200);   /* all parked, two frames each */
    int parked_frames = (int)sw_frames_live();
    for (int i = 0; i < 100; i++) sw_send_value(crashers[i], 0, sw_val_nil());
    wait_ms(200);
    int up = 0;
    for (int i = 0; i < 100; i++) {
        int s = atomic_load(&crashers[i]->state);
        if (s == SW_PROC_WAITING || s == SW_PROC_RUNNABLE || s == SW_PROC_RUNNING) up++;
    }
    CHECK(up == 0, "100 processes panicked after a resume (%d still live)", up);
    CHECK(parked_frames == 200 && sw_frames_live() == 0,
          "their frame chains were freed (%d parked, %ld left)", parked_frames, sw_frames_live());

    sw_shutdown(0);
    printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
