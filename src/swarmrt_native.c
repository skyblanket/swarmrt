/*
 * SwarmRT - Lightweight Process Runtime with Arena Allocator
 *
 * Key innovation: Arena allocator eliminates ALL syscalls from spawn hot path.
 * Single mmap at init, zero-syscall process spawn via lock-free slab + block pool.
 *
 * Before: ~3-4 us/spawn (mmap + mprotect + calloc + malloc per process)
 * After:  <1 us/spawn (atomic pop from pre-allocated arena)
 *
 * otonomy.ai
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
#include <stdint.h>
#include <stdatomic.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <process.h>
#else
  #include <unistd.h>
  #include <pthread.h>
  #include <signal.h>
  #include <time.h>
  #include <errno.h>
  #include <sys/mman.h>
  #include <sys/time.h>
  #ifdef __APPLE__
    #include <dispatch/dispatch.h>
  #endif
#endif
#include "swarmrt_native.h"
#include "swarmrt_ets.h"
#include "swarmrt_phase5.h"
#include "swarmrt_hotload.h"
#include "swarmrt_varena.h"   /* GC v1 per-process value arena */
#include <stddef.h>

/* The context-switch asm (swarmrt_asm.S) reads sw_process fields by HARDCODED
 * byte offset per architecture. Pin them so any struct-layout change that shifts
 * ctx/entry/arg is a COMPILE ERROR here instead of an intermittent crash. Keep
 * these in lockstep with the CTX_OFFSET/ENTRY_OFFSET/ARG_OFFSET defines in
 * swarmrt_asm.S. (New sw_process fields go at the END to avoid shifting these.) */
#if defined(__aarch64__) || defined(__arm64__)
_Static_assert(offsetof(struct sw_process, ctx)   == 0x70, "swarmrt_asm.S CTX_OFFSET (arm64) out of sync");
_Static_assert(offsetof(struct sw_process, entry) == 0xF0, "swarmrt_asm.S ENTRY_OFFSET (arm64) out of sync");
_Static_assert(offsetof(struct sw_process, arg)   == 0xF8, "swarmrt_asm.S ARG_OFFSET (arm64) out of sync");
#elif defined(__x86_64__)
_Static_assert(offsetof(struct sw_process, ctx)   == 0x70, "swarmrt_asm.S CTX_OFFSET (x86_64) out of sync");
_Static_assert(offsetof(struct sw_process, entry) == 0xC0, "swarmrt_asm.S ENTRY_OFFSET (x86_64) out of sync");
_Static_assert(offsetof(struct sw_process, arg)   == 0xC8, "swarmrt_asm.S ARG_OFFSET (x86_64) out of sync");
#endif

/* === Global State === */
sw_swarm_t *g_swarm = NULL;
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread sw_scheduler_t *tls_scheduler = NULL;
static __thread sw_process_t *tls_current = NULL;

/* Mailbox depth cap (see SW_MAILBOX_MAX_DEFAULT in the header). Plain global:
 * written exactly once in sw_init BEFORE any scheduler thread exists (ordered
 * by pthread_create), read-only afterwards — no atomics needed. 0 = unbounded. */
static int64_t g_mailbox_max = SW_MAILBOX_MAX_DEFAULT;
/* Total messages dropped by the cap (LOUD: also rate-limited stderr below). */
static _Atomic uint64_t g_mb_dropped;

/* Per-process memory quota (SW_PROC_MEM_MAX, bytes; 0 = unlimited — the
 * deliberate product default). Same publication contract as g_mailbox_max:
 * written once in sw_init before any scheduler thread exists. Enforced by
 * sw_varena_quota_check below (called from the varena grow/adopt cold paths);
 * scope is the per-process VALUE ARENA — the global-heap fallback (SW_GC_OFF,
 * interpreter values outside a fiber) is not metered. */
static size_t g_proc_mem_max = 0;

/* Max LOCAL message size (SW_MSG_MAX_BYTES, bytes; 0 = unlimited — the
 * deliberate product default: dist frames are already bounded by
 * SW_NODE_MAX_FRAME, this cap exists for operators hardening a node against
 * hostile/buggy senders). Same publication contract as g_mailbox_max.
 * Enforced in sw_send_tagged_msg on the message REGION's total_bytes — the
 * exact bytes the deep-copied value graph occupies. The global-heap fallback
 * (SW_GC_OFF / no sender arena) is not metered, same scope caveat as
 * SW_PROC_MEM_MAX. EXIT/DOWN ride deliver_signal and are exempt — a size cap
 * must never blind supervision. */
static size_t g_msg_max_bytes = 0;
/* Total messages dropped by the size cap (LOUD: rate-limited stderr below). */
static _Atomic uint64_t g_msgsize_dropped;

uint64_t sw_mailbox_dropped(void) {
    return atomic_load_explicit(&g_mb_dropped, memory_order_relaxed);
}

uint64_t sw_msgsize_dropped(void) {
    return atomic_load_explicit(&g_msgsize_dropped, memory_order_relaxed);
}

/* ── Graceful shutdown (Phase 4) ─────────────────────────────────────────
 * g_sw_draining        set the instant sw_shutdown_graceful begins; read by
 *                      sw_is_draining() → swarm_stats() so /readyz can report
 *                      NOT-ready during the drain window.
 * g_sw_shutdown_requested  set ONLY by the async-signal-safe SIGTERM/SIGINT
 *                      handler (or sw_request_shutdown); polled by the
 *                      main-thread wait loop (sw_wait_for_exit), which then runs
 *                      the drain OFF a scheduler thread. Both relaxed — they are
 *                      one-way flags, never used for synchronization; the actual
 *                      teardown handshake is sw_shutdown's join. */
static _Atomic int g_sw_draining = 0;
static _Atomic int g_sw_shutdown_requested = 0;

int sw_is_draining(void) {
    return atomic_load_explicit(&g_sw_draining, memory_order_relaxed);
}

int sw_shutdown_requested(void) {
    return atomic_load_explicit(&g_sw_shutdown_requested, memory_order_relaxed);
}

void sw_request_shutdown(void) {
    atomic_store_explicit(&g_sw_shutdown_requested, 1, memory_order_relaxed);
}

int sw_shutdown_grace_ms(void) {
    const char *e = getenv("SW_SHUTDOWN_GRACE_MS");
    if (e && *e) {
        long v = strtol(e, NULL, 10);
        if (v >= 0 && v <= 3600000) return (int)v;   /* clamp to [0, 1h] */
    }
    return 5000;
}

/* Async-signal-safe: touches ONLY a lock-free atomic. The FIRST signal requests
 * a graceful drain; a SECOND (impatient operator, or systemd escalating before
 * TimeoutStopSec) hard-exits immediately via _exit — also async-signal-safe.
 * Standard shell exit codes: 130 for SIGINT, 143 (128+SIGTERM) otherwise. */
static void _sw_term_handler(int sig) {
    if (atomic_exchange_explicit(&g_sw_shutdown_requested, 1,
                                 memory_order_relaxed)) {
        _exit(sig == SIGINT ? 130 : 143);
    }
}

void sw_install_shutdown_signals(void) {
    if (getenv("SW_NO_SIGNAL_SHUTDOWN")) return;
#ifndef _WIN32
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = _sw_term_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* no SA_RESTART: the polling wait re-checks promptly */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
#else
    signal(SIGTERM, _sw_term_handler);
    signal(SIGINT,  _sw_term_handler);
#endif
}

int sw_wait_for_exit(volatile int *done_flag, pthread_mutex_t *lock,
                     pthread_cond_t *cond) {
    pthread_mutex_lock(lock);
    while (!*done_flag && !sw_shutdown_requested()) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000L;   /* 50 ms poll — signal noticed promptly */
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(cond, lock, &ts);
    }
    int normal = *done_flag ? 1 : 0;
    pthread_mutex_unlock(lock);
    return normal;
}

/* Phase-4 observability counters (see the header). Crashes are bumped in
 * process_exit's abnormal path (the single teardown choke point — both
 * scheduler exit sites route through it); restarts by sw_note_restart()
 * from the supervisor restart-record functions. Relaxed ordering: these
 * are monotonic stats, never used for synchronization. */
static _Atomic uint64_t g_proc_crashes;
static _Atomic uint64_t g_sup_restarts;

uint64_t sw_proc_crashes(void) {
    return atomic_load_explicit(&g_proc_crashes, memory_order_relaxed);
}

uint64_t sw_restarts_total(void) {
    return atomic_load_explicit(&g_sup_restarts, memory_order_relaxed);
}

void sw_note_restart(void) {
    atomic_fetch_add_explicit(&g_sup_restarts, 1, memory_order_relaxed);
}

/* ── Structured crash log (SW_LOG_JSON=1, default OFF) ───────────────────
 * One JSON object per line on stderr for every ABNORMAL process exit:
 *   {"ev":"proc_crash","pid":N,"reason":R[,"msg":"…"][,"name":"…"],"ts":MS}
 * reason is the numeric exit reason; msg is the panic message when the exit
 * was a panic (JSON-escaped, truncated); name appears only while the process
 * is registered (the emit runs BEFORE process_exit unregisters it). Opt-in
 * for log shippers — the human-readable panic trace stays the default
 * surface. Emitted from process_exit (the single teardown choke point), so
 * it is allocation-free by design: stack buffers + one fprintf (a single
 * write keeps concurrent schedulers' records from interleaving mid-line). */
static _Atomic int g_log_json = -1;   /* -1 unresolved, 0 off, 1 on */

static int log_json_enabled(void) {
    int v = atomic_load_explicit(&g_log_json, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("SW_LOG_JSON");
        v = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
        atomic_store_explicit(&g_log_json, v, memory_order_relaxed);
    }
    return v;
}

/* Minimal JSON string escape into a fixed stack buffer (truncates).
 * Backslash + quote are escaped; control bytes become spaces — enough for
 * a diagnostic line without \uXXXX expansion or allocation. */
static void json_escape_buf(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    if (!src || cap == 0) { if (cap) dst[0] = '\0'; return; }
    for (size_t i = 0; src[i] && o + 2 < cap; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '"' || ch == '\\') { dst[o++] = '\\'; dst[o++] = (char)ch; }
        else if (ch < 0x20)          { dst[o++] = ' '; }
        else                         { dst[o++] = (char)ch; }
    }
    dst[o] = '\0';
}

static void emit_crash_json(sw_process_t *proc, int reason) {
    /* panic_msg is safe to read directly here: it is freed later in
     * process_destroy (same thread, after process_exit returns) and its
     * only writer is the process itself via sw_process_panic. */
    char msg_esc[192];
    json_escape_buf(proc->panic_msg, msg_esc, sizeof(msg_esc));

    /* Registered name: read under the registry rdlock — reg_entry is freed
     * only under that lock (the process_info discipline); copy out before
     * unlocking. */
    char name_esc[SW_REG_NAME_MAX * 2];
    name_esc[0] = '\0';
    int have_name = 0;
    if (g_swarm) {
        pthread_rwlock_rdlock(&g_swarm->registry.lock);
        sw_reg_entry_t *re = atomic_load_explicit(&proc->reg_entry, memory_order_acquire);
        if (re) { json_escape_buf(re->name, name_esc, sizeof(name_esc)); have_name = 1; }
        pthread_rwlock_unlock(&g_swarm->registry.lock);
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;

    char msg_field[224];
    msg_field[0] = '\0';
    if (proc->panic_msg)
        snprintf(msg_field, sizeof(msg_field), ",\"msg\":\"%s\"", msg_esc);
    char name_field[SW_REG_NAME_MAX * 2 + 16];
    name_field[0] = '\0';
    if (have_name)
        snprintf(name_field, sizeof(name_field), ",\"name\":\"%s\"", name_esc);

    fprintf(stderr, "{\"ev\":\"proc_crash\",\"pid\":%llu,\"reason\":%d%s%s,\"ts\":%llu}\n",
            (unsigned long long)proc->pid, reason, msg_field, name_field,
            (unsigned long long)ms);
}

/* Generated-code execution state (line/file/call-trace), PER PROCESS. `_sw_gen`
 * is pointed at the running process's gen_exec block on every context switch in
 * (below), so the generated _sw_current_line/_sw_current_file/_sw_trace* macros
 * (swarmrt_builtins_studio.h) reach the right process — a panic after a blocking
 * op then reports THIS process's location, not a fiber that shared the thread.
 * The fallback covers OOM (gen_exec calloc failed) and any code not running on a
 * fiber; it's a single thread-local, so its only failure mode is a wrong line in
 * a diagnostic banner from non-fiber code — never a memory-safety issue.
 *
 * Both are zero-initialized (a __thread initializer must be a compile-time
 * constant; a string-literal designated init / a TLS address is not). The
 * fallback's NULL current_file is fine — the panic readers guard on it — and
 * `_sw_gen` is assigned (to a process block or &_sw_gen_fallback) at every
 * context switch before any generated code runs, so it's never read while NULL:
 * generated line/file/trace writes and the studio.h panic readers both execute
 * only on a fiber, after switch-in. Belt-and-braces, it's set at scheduler-loop
 * entry too. */
static __thread sw_gen_exec_t _sw_gen_fallback;
__thread sw_gen_exec_t *_sw_gen;
/* Per-process usable stack size in bytes. 0 = use the built-in
 * SW_PROC_STACK_SIZE default (128KB; compiled path). Set BEFORE sw_init by
 * `swc run` so the interpreter's deep C-stack tree-walk runs on a large fiber
 * (it used to run on the 8MB OS main thread). Lazy mmap = no physical cost. */
size_t sw_proc_stack_size = 0;
/* GC v2: region handed to the next sw_spawn_opts to record on the child's
 * proc->spawn_region (pre-runnable). Set by sw_spawn_owned, consumed once. */
static __thread struct sw_value_arena *g_pending_spawn_region = NULL;

/* Per-process teardown hook handed to the next sw_spawn_opts to record on the
 * child's proc->on_destroy BEFORE it is runnable — so even a pre-trampoline kill
 * (the child killed before its entry fn ever runs to self-arm) reclaims the
 * spawn arg via process_destroy. Set by sw_spawn_dtor/sw_spawn_link_dtor,
 * consumed once. on_destroy_arg is the spawn arg. */
static __thread void (*g_pending_on_destroy)(void *) = NULL;
static __thread void *g_pending_on_destroy_arg = NULL;

/* Optional per-spawn pin set by sw_spawn_link so the child lands on a
 * specific scheduler (not the parent's). sw_spawn_opts honours this
 * when non-NULL and falls back to round-robin otherwise. NULL on
 * external (non-scheduler) threads. */
static __thread sw_scheduler_t *tls_spawn_override = NULL;

/* === Deadlock watchdog state === */
static pthread_t        g_watchdog_thread;
static volatile int     g_watchdog_stop = 0;
static int              g_watchdog_enabled = 1;   /* 0 when SW_DEADLOCK_DETECT=0 */
/* Shutdown wake for the watchdog. It used to sleep its interval in 100ms
 * nanosleep chunks polling g_watchdog_stop — so EVERY binary paid an avg
 * ~50ms (worst 100ms) at exit joining it. For a CLI tool whose whole run
 * is 10ms, that chunk WAS the startup-time story (Round-7 O5: hello
 * measured 110-145ms wall, ~99ms of it this join). A condvar makes
 * shutdown instant and removes the idle wakeups entirely. */
static pthread_mutex_t  g_watchdog_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_watchdog_cond = PTHREAD_COND_INITIALIZER;

/* Defined in swarmrt_io.c. Forward-declared here (rather than pulling in
 * swarmrt_io.h with its full port/event structs) so the watchdog can ask
 * the I/O subsystem whether any live port could still wake a process. */
int sw_io_active_port_count(void);

/*
 * watchdog_thread_fn — wakes periodically and checks for total deadlock.
 *
 * A "stuck" process is one that is:
 *   1. Not SW_PROC_FREE (slot is occupied).
 *   2. Not one of the runtime's own scheduler sched_proc stubs.
 *   3. In state SW_PROC_WAITING.
 *   4. Has an empty mailbox: sig_head == NULL AND priv_head == NULL.
 *
 * If every live non-scheduler process meets (3)+(4) we warn once per
 * interval.  We do NOT warn on an empty swarm (live_count == 0).
 *
 * All reads are best-effort — we hold no locks.  A false positive is
 * possible if a message is in flight at the exact moment we scan; that
 * is acceptable for a warn-only detector.
 */
static void *watchdog_thread_fn(void *arg) {
    (void)arg;

    /* Read interval from env (milliseconds, default 5000). */
    unsigned long interval_ms = 5000;
    const char *ms_env = getenv("SW_DEADLOCK_MS");
    if (ms_env && *ms_env) {
        long v = strtol(ms_env, NULL, 10);
        if (v >= 100) interval_ms = (unsigned long)v;
    }

    while (!g_watchdog_stop) {
        /* Wait one interval OR an instant shutdown signal — no chunked
         * polling (see g_watchdog_cond above for the boot-time story). */
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += (time_t)(interval_ms / 1000);
        dl.tv_nsec += (long)((interval_ms % 1000) * 1000000L);
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        pthread_mutex_lock(&g_watchdog_lock);
        while (!g_watchdog_stop) {
            if (pthread_cond_timedwait(&g_watchdog_cond, &g_watchdog_lock, &dl) != 0)
                break;   /* interval elapsed (ETIMEDOUT) — go scan */
        }
        pthread_mutex_unlock(&g_watchdog_lock);
        if (g_watchdog_stop) break;

        sw_swarm_t *sw = g_swarm;
        if (!sw || !sw->running) continue;

        sw_process_t *slab = (sw_process_t *)sw->arena.proc_slab;
        if (!slab) continue;
        uint32_t cap = sw->arena.proc_capacity;

        /* Build a small set of scheduler stub PIDs to exclude. */
        uint64_t sched_pids[SWARM_MAX_SCHEDULERS];
        uint32_t nsched = sw->num_schedulers;
        if (nsched > SWARM_MAX_SCHEDULERS) nsched = SWARM_MAX_SCHEDULERS;
        for (uint32_t s = 0; s < nsched; s++) {
            sw_scheduler_t *sc = sw->schedulers[s];
            sched_pids[s] = sc ? sc->sched_proc.pid : (uint64_t)-1;
        }

        int live_count  = 0;
        int stuck_count = 0;

        for (uint32_t i = 0; i < cap; i++) {
            sw_process_t *p = &slab[i];
            sw_proc_state_t st = p->state;   /* plain read — best-effort */

            if (st == SW_PROC_FREE) continue;

            /* Skip scheduler internal stub processes. */
            uint64_t pid = p->pid;
            int is_sched = 0;
            for (uint32_t s = 0; s < nsched; s++) {
                if (pid == sched_pids[s]) { is_sched = 1; break; }
            }
            if (is_sched) continue;

            live_count++;

            if (st == SW_PROC_WAITING) {
                /* Empty mailbox = sig_head NULL and priv_head NULL. */
                sw_msg_t *sig = (sw_msg_t *)atomic_load_explicit(
                    &p->mailbox.sig_head, memory_order_acquire);
                if (!sig && !p->mailbox.priv_head) {
                    stuck_count++;
                }
            }
        }

        /* Suppress the false positive for processes legitimately parked
         * on I/O: an idle TCP/HTTP server has every process waiting in
         * `receive` for the I/O thread to deliver an accept/data event.
         * That is not a deadlock — a live port can still wake them. */
        int active_ports = sw_io_active_port_count();

        if (live_count > 0 && stuck_count == live_count && active_ports == 0) {
            fprintf(stderr,
                "[swarmrt] WARNING: all %d process%s blocked in `receive`"
                " with an empty mailbox for >%lums — possible deadlock.\n"
                "[swarmrt]   Every live process is waiting for a message"
                " that no one is sending. Check that a sender exists and"
                " is reachable (right pid?).\n"
                "[swarmrt]   If a process may legitimately wait for a"
                " message that never arrives, give its `receive` a timeout"
                " clause: `receive { ... after MS { /* on timeout */ } }`.\n",
                live_count, live_count == 1 ? "" : "es", interval_ms);
            fflush(stderr);
        }
    }
    return NULL;
}

/* === Per-thread freelists (avoid malloc/free on hot path) === */
#define MSG_FREELIST_MAX 128
static __thread sw_msg_t *tls_msg_free = NULL;
static __thread int tls_msg_free_count = 0;

static inline sw_msg_t *msg_alloc(void) {
    sw_msg_t *m;
    if (tls_msg_free) {
        m = tls_msg_free;
        tls_msg_free = m->next;
        tls_msg_free_count--;
    } else {
        m = (sw_msg_t *)malloc(sizeof(sw_msg_t));
    }
    /* Ownership v2: a recycled freelist envelope must never carry a stale region/
     * pkind (would double-free or mis-free a foreign region). Zero them here so
     * every enqueue path starts clean; value sends set them in sw_send_tagged_msg. */
    if (m) { m->region = NULL; m->pkind = SW_PK_RAW; }
    return m;
}

static inline void msg_free(sw_msg_t *m) {
    if (tls_msg_free_count < MSG_FREELIST_MAX) {
        m->next = tls_msg_free;
        tls_msg_free = m;
        tls_msg_free_count++;
        return;
    }
    free(m);
}

/* Ownership v2: hand a popped message's payload to a C-LEVEL receiver
 * (sw_receive / sw_receive_any / sw_receive_tagged — used by tests, pmap, task,
 * remote delivery). The compiled-sw `receive` has its own loop that ADOPTS the
 * region; C consumers instead expect a standalone, free-able payload. So for a
 * SW_PK_VALUE message, materialize the graph onto the global heap (free-able)
 * and reclaim the region here — UNLESS `adopt` is set, in which case adopt the
 * region into this process's arena (caller keeps it, must NOT free it).
 * RAW payloads pass through unchanged. `adopt` is a PARAMETER (on the caller's
 * fiber stack), NEVER a thread-local: a TLS flag would leak across the context
 * switch that a blocking receive performs, into whatever fiber runs next on this
 * scheduler thread — corrupting an unrelated process's ownership. */
static inline void *msg_take_payload(sw_msg_t *m, int adopt) {
    void *p = m->payload;
    if (m->pkind == SW_PK_VALUE && m->region) {
        sw_value_arena_t *self = sw_self_varena();
        if (adopt && self) {
            sw_varena_adopt(self, m->region);   /* payload now lives in self's arena */
        } else {
            extern void *sw_val_deep_copy_global(void *);
            p = sw_val_deep_copy_global(p);     /* free-able global-heap copy */
            sw_varena_free_all(m->region);
        }
        m->region = NULL;
        m->pkind = SW_PK_RAW;
    }
    return p;
}

/* Public wrapper around msg_free for codegen — sw_msg_t and msg_free
 * are runtime-internal but the receive codegen needs to release the
 * envelope after pattern matching. See header for ownership notes. */
void sw_msg_release(sw_msg_t *m) {
    if (m) msg_free(m);
}

#define TIMER_FREELIST_MAX 64
static __thread sw_timer_t *tls_timer_free = NULL;
static __thread int tls_timer_free_count = 0;

static inline sw_timer_t *timer_alloc(void) {
    if (tls_timer_free) {
        sw_timer_t *t = tls_timer_free;
        tls_timer_free = t->next;
        tls_timer_free_count--;
        return t;
    }
    return (sw_timer_t *)malloc(sizeof(sw_timer_t));
}

static inline void timer_free(sw_timer_t *t) {
    if (tls_timer_free_count < TIMER_FREELIST_MAX) {
        t->next = tls_timer_free;
        tls_timer_free = t;
        tls_timer_free_count++;
        return;
    }
    free(t);
}

/* === Timer for preemption (macOS uses dispatch, Linux POSIX timer, Windows timer queue) === */
#ifdef __APPLE__
static dispatch_source_t g_preempt_timer;
#elif defined(_WIN32)
static HANDLE g_preempt_timer_queue = NULL;
static HANDLE g_preempt_timer = NULL;
#else
static timer_t g_preempt_timer;
#endif
#ifndef _WIN32
/* Used only on the non-Apple sigaction preemption path; unused on macOS (which
 * uses a dispatch_source timer), so mark unused to keep the build warning-clean. */
static struct sigaction g_old_sigaction __attribute__((unused));
#endif

/* === Forward Declarations === */
static void *scheduler_main(void *arg);
static void scheduler_loop(sw_scheduler_t *sched);
static void process_exit(sw_process_t *proc, int reason);
static void process_destroy(sw_process_t *proc);
static void fire_timers(void);
static void registry_remove_proc(sw_process_t *proc);
#ifdef _WIN32
static VOID CALLBACK preempt_handler(PVOID param, BOOLEAN fired);
#else
static void preempt_handler(int sig, siginfo_t *info, void *context);
#endif
static void mailbox_drain(sw_mailbox_t *mb);
static inline void mailbox_wake(sw_process_t *to);

/* ============================================================================
 * ARENA ALLOCATOR
 *
 * Memory layout (single mmap):
 * ┌────────────────────────────────────────────┐
 * │ Process Slab: N * sizeof(sw_process_t)     │
 * ├────────────────────────────────────────────┤
 * │ Block Pool: M * 2KB blocks (process heaps) │
 * ├────────────────────────────────────────────┤
 * │ Free PID Stack: N * 4B                     │
 * ├────────────────────────────────────────────┤
 * │ Free Block Stack: M * 4B                   │
 * └────────────────────────────────────────────┘
 * ============================================================================ */

int sw_arena_init(sw_arena_t *arena, uint32_t max_procs) {
    uint32_t num_parts = g_swarm->num_schedulers;
    if (num_parts == 0) num_parts = 1;
    if (num_parts > SW_MAX_PARTITIONS) num_parts = SW_MAX_PARTITIONS;

    /* Calculate sizes */
    size_t proc_slab_size = (size_t)max_procs * sizeof(sw_process_t);
    size_t block_size = SWARM_HEAP_MIN_SIZE * sizeof(uint64_t);  /* 2KB */
    uint32_t block_count = max_procs;  /* 1 block per process */
    size_t block_pool_size = (size_t)block_count * block_size;
    /* Each partition needs max_procs capacity (worst case: all slots return
     * to one partition). Total stack memory = num_parts * max_procs * 4B. */
    size_t free_pid_size = (size_t)num_parts * max_procs * sizeof(uint32_t);
    size_t free_block_size = (size_t)num_parts * block_count * sizeof(uint32_t);

    /* Align everything to page boundaries */
    size_t total = proc_slab_size + block_pool_size + free_pid_size + free_block_size;
    total = (total + 4095) & ~(size_t)4095;  /* Page-align */

    /* Single allocation — one syscall to rule them all */
#ifdef _WIN32
    uint8_t *mem = (uint8_t *)VirtualAlloc(NULL, total,
                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        fprintf(stderr, "[SwarmRT] arena VirtualAlloc failed (%lu)\n", GetLastError());
        return -1;
    }
#else
    uint8_t *mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("[SwarmRT] arena mmap failed");
        return -1;
    }
#endif

    arena->base = mem;
    arena->size = total;

    /* Carve regions */
    uint8_t *cursor = mem;

    /* Region 1: Process slab */
    arena->proc_slab = (void *)cursor;
    arena->proc_capacity = max_procs;
    cursor += proc_slab_size;

    /* Region 2: Block pool (2KB blocks for heaps) */
    arena->block_base = cursor;
    arena->block_size = (uint32_t)block_size;
    arena->block_count = block_count;
    cursor += block_pool_size;

    /* Region 3: PID free stacks (partitioned) */
    arena->pid_stack_base = (uint32_t *)cursor;
    cursor += free_pid_size;

    /* Region 4: Block free stacks (partitioned) */
    arena->block_stack_base = (uint32_t *)cursor;

    /* Partition the free stacks across schedulers */
    arena->num_partitions = num_parts;
    uint32_t pids_per_part = max_procs / num_parts;
    uint32_t blocks_per_part = block_count / num_parts;
    for (uint32_t p = 0; p < num_parts; p++) {
        sw_arena_partition_t *part = &arena->partitions[p];

        /* Each partition gets max_procs capacity for worst-case imbalance */
        part->free_pids = arena->pid_stack_base + (size_t)p * max_procs;
        part->pid_capacity = max_procs;
        part->pid_top = 0;

        part->free_blocks = arena->block_stack_base + (size_t)p * block_count;
        part->block_capacity = block_count;
        part->block_top = 0;

        part->lock = (sw_spinlock_t)SW_SPINLOCK_INIT;

        /* Slots filled below (interleaved for cache locality) */
    }

    /* Contiguous partition ranges — each partition gets a dense block of slots.
     * Local spawn (from within a scheduler) pops consecutive slots with
     * 472-byte stride — perfect for hardware prefetcher. */
    for (uint32_t p = 0; p < num_parts; p++) {
        sw_arena_partition_t *part = &arena->partitions[p];
        uint32_t pid_start = p * pids_per_part;
        uint32_t pid_end = (p == num_parts - 1) ? max_procs : pid_start + pids_per_part;
        for (uint32_t i = pid_start; i < pid_end; i++) {
            part->free_pids[part->pid_top++] = i;
        }
        uint32_t blk_start = p * blocks_per_part;
        uint32_t blk_end = (p == num_parts - 1) ? block_count : blk_start + blocks_per_part;
        for (uint32_t i = blk_start; i < blk_end; i++) {
            part->free_blocks[part->block_top++] = i;
        }
    }

    /* Pre-initialize mailbox state.
     * Each mailbox uses a lock-free LIFO signal stack + private FIFO queue. */
    sw_process_t *slab = (sw_process_t *)arena->proc_slab;
    for (uint32_t i = 0; i < max_procs; i++) {
        sw_mailbox_t *mb = &slab[i].mailbox;
        atomic_store(&mb->sig_head, NULL);
        mb->priv_head = NULL;
        mb->priv_tail = NULL;
        atomic_store(&mb->waiting, 0);
        mb->count = 0;
        atomic_store(&slab[i].mb_len, 0);

        /* Per-slot ctx_lock + generation — guards process_init_arena
         * against concurrent swap-in. See sw_safe_swap_into for the
         * read side. Lock value zero-initializes correctly for both
         * os_unfair_lock (macOS) and pthread_mutex (Linux) — the
         * arena is calloc-zeroed — but we set it explicitly to make
         * the intent visible. */
        slab[i].ctx_lock = (sw_spinlock_t)SW_SPINLOCK_INIT;
        atomic_store(&slab[i].generation, 0);
    }

    /* PID counter starts at 0 */
    /* Start at 1 — pid 0 is reserved as the "no pid" sentinel that
     * sw_find_by_pid rejects. With cross-node messaging in play, the
     * very first spawned process (the wrapper around user main) would
     * otherwise get pid 0 and any remote reply to `self()` would
     * vanish on lookup. */
    atomic_store(&arena->next_pid, 1);

    return 0;
}

/* ============================================================================
 * PROCESS LIFECYCLE (Arena-backed, zero-syscall)
 * ============================================================================ */

/*
 * process_init_arena: Initialize a process using arena-allocated memory.
 * No mmap, no malloc, no calloc. Just pointer arithmetic + atomic ops.
 */
static int process_init_arena(sw_process_t *proc, uint32_t block_idx,
                              void (*entry)(void*), void *arg) {
    sw_arena_t *arena = &g_swarm->arena;

    /* Point heap at arena block — zero-copy, no malloc */
    uint64_t *block = (uint64_t *)(arena->block_base +
                                    (size_t)block_idx * arena->block_size);
    proc->heap.start = block;
    proc->heap.top = block;
    proc->heap.end = block + SWARM_HEAP_MIN_SIZE;
    proc->heap.size = SWARM_HEAP_MIN_SIZE;
    proc->heap.old_heap = NULL;
    proc->heap.old_top = NULL;
    proc->heap.old_size = 0;
    proc->heap.gen_gcs = 0;
    proc->heap.arena_backed = 1;  /* arena-allocated — never free() */

    proc->htop = block;
    proc->stop = NULL;
    proc->heap_block_idx = block_idx;

    /* GC v1 LIVE: per-process value arena. Every sw_val_t this fiber builds
     * lives here and is freed wholesale in process_destroy. Every value that
     * escapes the process (send/spawn/ETS/supervisor/timer/dist) is deep-copied
     * to the global heap at the boundary, so freeing this arena is safe. NULL
     * on OOM → val_alloc falls back to the global heap (pre-GC behaviour).
     * SW_GC_OFF=1 in the environment reverts to that pre-GC behaviour wholesale
     * (escape hatch + A/B harness; sampled once, cached). */
    {
        static int gc_off = -1;
        if (gc_off < 0) gc_off = getenv("SW_GC_OFF") ? 1 : 0;
        proc->varena = gc_off ? NULL : sw_varena_create(8192);
    }
    proc->spawn_region = NULL;   /* set by sw_spawn_owned before the child runs */
    proc->gen_error = NULL;      /* per-process generated-error slot (see sw_self_error_slot) */
    proc->try_chain = NULL;      /* per-process compiled try/catch chain (see sw_self_try_chain) */

    /* Per-process generated line/file/call-trace state. Lazily allocated and KEPT
     * WITH THE SLAB SLOT across lifetimes (like stack_mem below): reset here, freed
     * only at slab teardown. On OOM it stays NULL and the context switch falls back
     * to a thread-local (diagnostic-only). _sw_gen is pointed here on switch-in. */
    if (!proc->gen_exec) proc->gen_exec = (sw_gen_exec_t *)calloc(1, sizeof(sw_gen_exec_t));
    if (proc->gen_exec) {
        memset(proc->gen_exec, 0, sizeof(sw_gen_exec_t));
        proc->gen_exec->current_file = "<unknown>";
    }

    /* Optional per-process teardown hook — cleared on every (re)use of the slot
     * so a reused slot never inherits a stale destructor (set later by e.g. a
     * supervisor child fiber). */
    proc->on_destroy = NULL;
    proc->on_destroy_arg = NULL;

    /* Core fields */
    proc->entry = entry;
    proc->arg = arg;

    /* Allocate process stack with guard page (lazy — reuse across lifetimes).
     * Layout: [GUARD PAGE | usable stack ... ]
     * Stack grows downward; guard page at bottom catches overflow via SIGSEGV. */
#define SW_PROC_STACK_SIZE  (128 * 1024) /* 128KB usable per process stack.
     * The runtime embeds the tree-walking interpreter, and tools run it ON a
     * process fiber (tool_call -> sw_lang_call -> eval). Parsing (the 8KB-token,
     * ~14KB/frame hog) is offloaded to an 8MB helper thread in tool_define, so
     * the fiber only carries eval — but eval is still recursive, so 64KB was
     * tight. 128KB gives eval comfortable headroom while staying lightweight
     * (stacks are lazily mmap'd; physical = touched pages only). */
    if (!proc->stack_mem) {
        /* Per-process usable stack. Defaults to SW_PROC_STACK_SIZE (128KB) so
         * the compiled path is unchanged (millions of lightweight processes).
         * `swc run` runs the WHOLE tree-walking interpreter on the root
         * process fiber, which recurses deeply on the C stack (no TCO), so it
         * sets sw_proc_stack_size large (like the 8MB OS main thread it used
         * to run on) BEFORE sw_init. Lazy mmap means physical = touched pages,
         * so a large reservation costs nothing for shallow programs. */
        size_t usable = sw_proc_stack_size ? sw_proc_stack_size
                                           : (size_t)SW_PROC_STACK_SIZE;
#ifdef _WIN32
        long page_size = 4096;
        size_t total = (size_t)page_size + usable;
        total = (total + page_size - 1) & ~(page_size - 1);
        void *mem = VirtualAlloc(NULL, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) { return -1; }
        /* Bottom page is guard — no access */
        DWORD old_prot;
        VirtualProtect(mem, page_size, PAGE_NOACCESS, &old_prot);
#else
        long page_size = sysconf(_SC_PAGESIZE);
        size_t total = (size_t)page_size + usable;
        /* Round up to page alignment */
        total = (total + page_size - 1) & ~(page_size - 1);
        void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) { return -1; }
        /* Bottom page is guard — no access */
        mprotect(mem, page_size, PROT_NONE);
#endif
        proc->stack_mem = (uint8_t *)mem + page_size;  /* usable region */
        proc->stack_size = total - page_size;
    }

    /* Initialize context for first context switch → trampoline.
     *
     * ctx_lock + generation bump close the slot-reuse race (R2-#4):
     * a scheduler that picked this slot before reuse is mid-
     * sw_safe_swap_into, which holds the same lock to copy ctx to a
     * stack-local before calling asm. If the scheduler's expected
     * generation no longer matches what we set here, it skips the
     * swap. See sw_safe_swap_into for the read side. */
    sw_spin_lock(&proc->ctx_lock);
    atomic_fetch_add_explicit(&proc->generation, 1, memory_order_release);

    memset(&proc->ctx, 0, sizeof(sw_context_t));
    uint8_t *stack_top = (uint8_t *)proc->stack_mem + proc->stack_size;
    stack_top = (uint8_t *)((uintptr_t)stack_top & ~0xFULL);  /* 16-byte align */
#ifdef __aarch64__
    proc->ctx.sp = (uint64_t)stack_top;
    proc->ctx.pc = (uint64_t)sw_process_trampoline;
    proc->ctx.x19 = (uint64_t)proc;  /* Trampoline reads proc from x19 */
#else
    /* Pre-push trampoline address onto the stack so `ret` in context_swap
     * jumps to the trampoline on first switch (same mechanism as resuming). */
    stack_top -= 8;
    *(uint64_t *)stack_top = (uint64_t)sw_process_trampoline;
    proc->ctx.rsp = (uint64_t)stack_top;
    proc->ctx.r12 = (uint64_t)proc;  /* Trampoline reads proc from r12 */
#endif
    proc->ctx.stack_base = (uint64_t)stack_top;
    proc->ctx.stack_limit = (uint64_t)proc->stack_mem;
    sw_spin_unlock(&proc->ctx_lock);
    atomic_store_explicit(&proc->state, SW_PROC_RUNNABLE, memory_order_relaxed);
    proc->priority = SW_PRIO_NORMAL;
    proc->fcalls = SWARM_CONTEXT_REDS;
    proc->flags = 0;

    /* Mailbox — reset signal stack + private queue for reuse */
    atomic_store(&proc->mailbox.sig_head, NULL);
    proc->mailbox.priv_head = NULL;
    proc->mailbox.priv_tail = NULL;
    atomic_store(&proc->mailbox.waiting, 0);
    proc->mailbox.count = 0;
    atomic_store_explicit(&proc->mb_len, 0, memory_order_relaxed);

    /* rq_next/rq_prev are NOT set here — they're set by sw_add_to_runq
     * under the run queue lock. Setting them here races with the queue
     * if this slot was just returned by another scheduler thread. */

    /* Links & Monitors */
    proc->parent = NULL;
    proc->links = NULL;
    proc->monitors_me = NULL;
    proc->my_monitors = NULL;
    proc->kill_flag = 0;
    proc->exit_reason = 0;
    proc->reg_entry = NULL;
    proc->ets_tables = NULL;

    /* Stats */
    proc->reductions_done = 0;
    proc->context_switches = 0;
    proc->messages_sent = 0;
    proc->messages_recv = 0;

    return 0;
}

/*
 * process_destroy: Return arena resources, clean up mailbox.
 * No munmap, no free (except mailbox messages which are malloc'd).
 */
static void process_destroy(sw_process_t *proc) {
    /* Save arena indices BEFORE returning — once pushed, another thread
     * can immediately reuse this slot and reinitialize it. */
    uint32_t block_idx = proc->heap_block_idx;
    uint32_t slot = proc->arena_slot;

    /* Per-process teardown hook FIRST (snapshot-then-clear-then-call so a
     * re-entrant/double destroy can't fire it twice). Runs before the arena /
     * region frees and the slot recycle below — the hook touches only global-heap
     * state (e.g. a supervisor child's start-closure copy), never this arena. No
     * lock is held here (process_exit already released link_lock). */
    {
        void (*od)(void *) = proc->on_destroy;
        void *oda = proc->on_destroy_arg;
        proc->on_destroy = NULL;
        proc->on_destroy_arg = NULL;
        if (od) od(oda);
    }

    /* Free any remaining messages in signal stack. EXIT/DOWN payloads
     * are sw_signal_t with an owned reason_str — must free that too. */
    sw_msg_t *sig = atomic_exchange(&proc->mailbox.sig_head, NULL);
    while (sig) {
        sw_msg_t *next = (sw_msg_t *)atomic_load_explicit(&sig->sig_next, memory_order_relaxed);
        /* Ownership v2: a VALUE message's payload graph is OWNED by its region —
         * bulk-free the region and do NOT free(payload). RAW payloads (signals,
         * gen_server/port structs) keep their exact existing free path. */
        if (sig->pkind == SW_PK_VALUE) {
            if (sig->region) sw_varena_free_all(sig->region);
        } else if (sig->payload) {
            if (sig->tag == SW_TAG_EXIT || sig->tag == SW_TAG_DOWN) {
                sw_signal_t *s = (sw_signal_t *)sig->payload;
                free(s->reason_str);
            }
            free(sig->payload);
        }
        msg_free(sig);
        sig = next;
    }
    /* Free any remaining messages in private queue (same shape). */
    sw_msg_t *msg = proc->mailbox.priv_head;
    while (msg) {
        sw_msg_t *next = msg->next;
        if (msg->pkind == SW_PK_VALUE) {
            if (msg->region) sw_varena_free_all(msg->region);
        } else if (msg->payload) {
            if (msg->tag == SW_TAG_EXIT || msg->tag == SW_TAG_DOWN) {
                sw_signal_t *s = (sw_signal_t *)msg->payload;
                free(s->reason_str);
            }
            free(msg->payload);
        }
        msg_free(msg);
        msg = next;
    }
    proc->mailbox.priv_head = NULL;
    proc->mailbox.priv_tail = NULL;
    proc->mailbox.count = 0;
    atomic_store_explicit(&proc->mb_len, 0, memory_order_relaxed);

    /* Free the panic_msg string set by sw_process_panic. UNDER link_lock:
     * a late sw_monitor on this (already-EXITING/FREE) process reads
     * panic_msg in its already-dead delivery path under the same lock, so
     * the free must serialise with it or the monitor reads freed memory.
     * NULL-under-lock means the monitor sees either a valid pointer or
     * NULL, never freed-but-non-NULL. */
    /* The varena/spawn_region DETACH also happens inside this same critical
     * section (snapshot-then-NULL under link_lock, free after unlock): the
     * process_info builtin reads proc->varena->total_bytes cross-thread for
     * its 'memory' stat, under link_lock. Detaching under the lock gives it
     * the same guarantee panic_msg has — a reader holding link_lock sees
     * either a live arena header or NULL, never freed-but-non-NULL. The
     * frees stay outside the lock (O(chunks), no need to hold it). */
    pthread_mutex_lock(&g_swarm->link_lock);
    char *_pm = proc->panic_msg;
    proc->panic_msg = NULL;
    struct sw_value_arena *_va = proc->varena;
    proc->varena = NULL;
    struct sw_value_arena *_sr = proc->spawn_region;
    proc->spawn_region = NULL;
    pthread_mutex_unlock(&g_swarm->link_lock);
    free(_pm);

    /* GC v1: reclaim this process's whole value arena in O(chunks). Runs after
     * the fiber has returned, so there are no live sw_val_t* C-stack roots.
     * Every value that escaped this process was deep-copied to the global heap
     * at the send/spawn/ETS boundary, so nothing else aliases these chunks. */
    if (_va)
        sw_varena_free_all(_va);

    /* GC v2: a spawn region still attached means the child was killed BEFORE its
     * trampoline adopted it (pre-start kill) — reclaim it here so it isn't leaked. */
    if (_sr)
        sw_varena_free_all(_sr);

    /* Return block and slot to the current scheduler's partition
     * immediately. The slot-reuse race (R2-#4) is now closed by the
     * per-slot ctx_lock + generation counter on the swap-in path, not
     * by deferring the free here. See sw_safe_swap_into in scheduler_loop
     * for the read side, and process_init_arena for the write side. */
    sw_scheduler_t *sched = tls_scheduler;
    uint32_t part_id = sched ? sched->id : 0;
    if (part_id >= g_swarm->arena.num_partitions) part_id = 0;
    sw_arena_partition_t *part = &g_swarm->arena.partitions[part_id];
    sw_spin_lock(&part->lock);
    part_push_pid(part, slot);
    part_push_block(part, block_idx);
    sw_spin_unlock(&part->lock);
}

/* ============================================================================
 * PREEMPTION (Reduction-based Scheduling)
 * ============================================================================ */

#ifdef _WIN32
static VOID CALLBACK preempt_handler(PVOID param, BOOLEAN fired) {
    (void)param;
    (void)fired;
#else
static void preempt_handler(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)info;
    (void)context;
#endif

    sw_scheduler_t *sched = tls_scheduler;
    sw_process_t *current = tls_current;

    if (!sched || !current) return;

    /* Force yield - decrement reductions to trigger reschedule */
    current->fcalls = 0;
}

/* DEAD CODE — never called (discovered in the Round-7 LSan work, June
 * 2026). The intended design was a 1ms timer zeroing the running
 * process's fcalls so CPU-bound code gets descheduled, but no call site
 * was ever wired and the only real preemption is the reduction check at
 * compiled tail-loop backedges (sw_check_reds at `goto _tail`). Wiring
 * this up WOULD tighten preemption latency for long single turns — but
 * it is a behavior change that needs the full storm/stress battery and
 * a signal-vs-sanitizer interaction review (a 1ms SIGALRM storm is
 * hostile to LSan's stop-the-world), so it stays off deliberately.
 * Kept (not deleted) as the blueprint for that future change. */
__attribute__((unused))
static void setup_preemption(void) {
#ifdef __APPLE__
    g_preempt_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                              dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0));
    dispatch_source_set_timer(g_preempt_timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                              1000000ull, 1000ull);  /* 1ms interval */
    dispatch_source_set_event_handler(g_preempt_timer, ^{
        /* Preemption signal */
    });
    dispatch_resume(g_preempt_timer);
#elif defined(_WIN32)
    /* Windows: timer queue fires callback every 1ms */
    g_preempt_timer_queue = CreateTimerQueue();
    if (g_preempt_timer_queue) {
        CreateTimerQueueTimer(&g_preempt_timer, g_preempt_timer_queue,
            (WAITORTIMERCALLBACK)preempt_handler, NULL,
            0, 1, WT_EXECUTEDEFAULT);
    }
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = preempt_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGALRM, &sa, &g_old_sigaction);

    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_signo = SIGALRM;

    timer_create(CLOCK_MONOTONIC, &sev, &g_preempt_timer);

    struct itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 1000000;
    its.it_interval = its.it_value;

    timer_settime(g_preempt_timer, 0, &its, NULL);
#endif
}

/* Called by process to check if it should yield */
int sw_check_reds(void) {
    sw_process_t *proc = tls_current;
    if (!proc) return 0;

    proc->fcalls--;
    proc->reductions_done++;

    if (proc->fcalls <= 0) {
        return 1;
    }
    return 0;
}

/* ============================================================================
 * SCHEDULING
 * ============================================================================ */

/*
 * sw_add_to_runq: Lock-free Vyukov MPSC push.
 *
 * Cost: 1 atomic_exchange (~10ns on ARM64) + 1 atomic_store (~5ns).
 * No mutex. No kernel call. Pure lock-free scheduling.
 *
 * The atomic_exchange on tail linearizes concurrent pushes — each producer
 * gets a unique "prev" to link through. The subsequent store to prev->rq_next
 * completes the link (consumer waits for this to become non-NULL).
 */
/* SW_SCHED_TRACE=1 — 1Hz scheduler counter telemetry on stderr, for
 * diffing a wedged run against a healthy one (the slope_message
 * phase-lock hunt). Relaxed atomic counters, all increments gated behind
 * the flag; zero cost when off. */
static int g_sched_trace = -1;
static _Atomic uint64_t g_tr_enq, g_tr_sig, g_tr_idle_seen,
                        g_tr_park, g_tr_park_signal, g_tr_park_timeout,
                        g_tr_spin_hit, g_tr_guard_hit;
/* Wedge autopsy: when SW_SCHED_TRACE=1 and enqueues flatline for 3
 * consecutive seconds while live processes exist, dump every live
 * process's lifecycle state, waiting flag, and mailbox pointers, plus
 * each scheduler's runq internals — the exact stuck state of the
 * spin-gated P1 deadlock, labeled. Walks the slab unsynchronized
 * (stall-time only; same warn-only contract as the watchdog). */
/* SW_SCHED_TRACE=2: lock-free event ring for the P1 hunt. Every
 * protocol-relevant transition appends {seq, ev, pid, aux}; the autopsy
 * dumps the tail. Relaxed fetch_add slot claim — order is the global
 * seq, exactly what the interleaving analysis needs. */
#define SW_TRACE_RING 4096
typedef struct { uint64_t seq; uint8_t ev; uint64_t pid; uint64_t aux; } sw_trev_t;
static sw_trev_t g_trev[SW_TRACE_RING];
static _Atomic uint64_t g_trev_seq;
enum { TREV_PUSH = 1, TREV_WAKE1, TREV_WAKE0, TREV_WSET, TREV_WCLR_SELF,
       TREV_DRAIN_HIT, TREV_DRAIN_MISS, TREV_PARKSWAP, TREV_ENQ,
       TREV_PICK, TREV_SWAPFAIL, TREV_RUN };
static inline void trev(uint8_t ev, uint64_t pid, uint64_t aux) {
    if (g_sched_trace < 2) return;
    uint64_t s = atomic_fetch_add_explicit(&g_trev_seq, 1, memory_order_relaxed);
    sw_trev_t *e = &g_trev[s % SW_TRACE_RING];
    e->seq = s; e->ev = ev; e->pid = pid; e->aux = aux;
}
static const char *trev_name(uint8_t ev) {
    switch (ev) {
    case TREV_PUSH: return "PUSH";       case TREV_WAKE1: return "WAKE->enq";
    case TREV_WAKE0: return "WAKE-noop"; case TREV_WSET: return "WSET";
    case TREV_WCLR_SELF: return "WCLR-self"; case TREV_DRAIN_HIT: return "DRAIN-hit";
    case TREV_DRAIN_MISS: return "DRAIN-miss"; case TREV_PARKSWAP: return "PARK-swap";
    case TREV_ENQ: return "ENQ";         case TREV_PICK: return "PICK";
    case TREV_SWAPFAIL: return "SWAPFAIL"; case TREV_RUN: return "RUN";
    default: return "?";
    }
}
static void trev_dump(void) {
    uint64_t end = atomic_load_explicit(&g_trev_seq, memory_order_relaxed);
    uint64_t start = end > 64 ? end - 64 : 0;
    fprintf(stderr, "[sched-autopsy] last %llu protocol events:\n",
            (unsigned long long)(end - start));
    for (uint64_t s = start; s < end; s++) {
        sw_trev_t e = g_trev[s % SW_TRACE_RING];
        if (e.seq != s) continue;   /* overwritten mid-read */
        fprintf(stderr, "  #%llu %-10s pid=%llu aux=%llu\n",
                (unsigned long long)e.seq, trev_name(e.ev),
                (unsigned long long)e.pid, (unsigned long long)e.aux);
    }
}

static void sched_trace_autopsy(void) {
    if (!g_swarm) return;
    sw_process_t *slab = (sw_process_t *)g_swarm->arena.proc_slab;
    if (!slab) return;
    uint32_t cap = g_swarm->arena.proc_capacity;
    fprintf(stderr, "[sched-autopsy] === enqueues flatlined; dumping live state ===\n");
    for (uint32_t i = 0; i < cap; i++) {
        if (slab[i].state == SW_PROC_FREE) continue;
        int is_stub = 0;
        for (uint32_t s = 0; s < g_swarm->num_schedulers; s++)
            if (g_swarm->schedulers[s] && &g_swarm->schedulers[s]->sched_proc == &slab[i]) is_stub = 1;
        if (is_stub) continue;
        fprintf(stderr,
            "[sched-autopsy] slot=%u pid=%llu state=%d waiting=%d sig_head=%p "
            "priv_head=%p count=%u kill=%d sched=%u rq_next=%p\n",
            i, (unsigned long long)slab[i].pid, (int)slab[i].state,
            atomic_load_explicit(&slab[i].mailbox.waiting, memory_order_relaxed),
            (void *)atomic_load_explicit(&slab[i].mailbox.sig_head, memory_order_relaxed),
            (void *)slab[i].mailbox.priv_head, slab[i].mailbox.count,
            slab[i].kill_flag,
            slab[i].scheduler ? slab[i].scheduler->id : 9999,
            (void *)atomic_load_explicit(&slab[i].rq_next, memory_order_relaxed));
    }
    for (uint32_t s = 0; s < g_swarm->num_schedulers; s++) {
        sw_scheduler_t *sc = g_swarm->schedulers[s];
        if (!sc) continue;
        for (int prio = 0; prio < SW_PRIO_NUM; prio++) {
            sw_process_t *head = sc->runq.heads[prio];
            sw_process_t *tail = atomic_load_explicit(&sc->runq.tails[prio], memory_order_relaxed);
            int head_is_stub = (head == &sc->runq.stubs[prio]);
            if (!head_is_stub || tail != head)
                fprintf(stderr,
                    "[sched-autopsy] sched=%u prio=%d head=%p%s tail=%p head->rq_next=%p idle=%d\n",
                    s, prio, (void *)head, head_is_stub ? "(stub)" : "",
                    (void *)tail,
                    head ? (void *)atomic_load_explicit(&head->rq_next, memory_order_relaxed) : NULL,
                    atomic_load_explicit(&sc->runq.idle, memory_order_relaxed));
        }
    }
    fprintf(stderr, "[sched-autopsy] === end ===\n");
}

static void *sched_trace_fn(void *arg) {
    (void)arg;
    uint64_t p[8] = {0};
    int flat_secs = 0, autopsied = 0;
    while (g_swarm && g_swarm->running) {
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
        uint64_t c[8] = {
            atomic_load(&g_tr_enq), atomic_load(&g_tr_sig),
            atomic_load(&g_tr_idle_seen), atomic_load(&g_tr_park),
            atomic_load(&g_tr_park_signal), atomic_load(&g_tr_park_timeout),
            atomic_load(&g_tr_spin_hit), atomic_load(&g_tr_guard_hit),
        };
        fprintf(stderr,
            "[sched-trace] enq=%llu sig=%llu idle_seen=%llu park=%llu "
            "park_sig=%llu park_to=%llu spin_hit=%llu guard_hit=%llu\n",
            (unsigned long long)(c[0]-p[0]), (unsigned long long)(c[1]-p[1]),
            (unsigned long long)(c[2]-p[2]), (unsigned long long)(c[3]-p[3]),
            (unsigned long long)(c[4]-p[4]), (unsigned long long)(c[5]-p[5]),
            (unsigned long long)(c[6]-p[6]), (unsigned long long)(c[7]-p[7]));
        /* Stall detector → one autopsy per episode (enq delta == 0). */
        if (c[0] - p[0] == 0) {
            if (++flat_secs >= 3 && !autopsied) { sched_trace_autopsy(); autopsied = 1; }
        } else {
            flat_secs = 0; autopsied = 0;
        }
        memcpy(p, c, sizeof(p));
    }
    return NULL;
}
static void sched_trace_maybe_start(void) {
    if (g_sched_trace < 0) {
        const char *e = getenv("SW_SCHED_TRACE");
        g_sched_trace = e ? atoi(e) : 0;   /* 1=counters, 2=+event ring */
        if (g_sched_trace) {
            pthread_t t;
            pthread_create(&t, NULL, sched_trace_fn, NULL);
            pthread_detach(t);
        }
    }
}

void sw_add_to_runq(sw_runq_t *rq, sw_process_t *proc) {
    uint32_t prio = proc->priority;
    if (prio >= SW_PRIO_NUM) prio = SW_PRIO_NORMAL;

    /* NOTE: state must be set by the CALLER before calling this function.
     * Setting state here races with the receiver's final-drain self-resume path
     * (see mailbox_wake + sw_receive double-enqueue fix). */
    atomic_store_explicit(&proc->rq_next, NULL, memory_order_relaxed);

    /* Vyukov MPSC push: exchange tail, link prev->next */
    sw_process_t *prev = atomic_exchange_explicit(&rq->tails[prio], proc,
                                                    memory_order_acq_rel);
    atomic_store_explicit(&prev->rq_next, proc, memory_order_release);

    /* Wake scheduler if it was idle (rare — only when queue was empty) */
    if (g_sched_trace >= 1) atomic_fetch_add_explicit(&g_tr_enq, 1, memory_order_relaxed);
    trev(TREV_ENQ, proc->pid, (uint64_t)(uintptr_t)rq);
    if (atomic_load_explicit(&rq->idle, memory_order_relaxed)) {
        if (g_sched_trace >= 1) atomic_fetch_add_explicit(&g_tr_idle_seen, 1, memory_order_relaxed);
        pthread_mutex_lock(&rq->idle_lock);
        pthread_cond_signal(&rq->idle_cond);
        pthread_mutex_unlock(&rq->idle_lock);
        if (g_sched_trace >= 1) atomic_fetch_add_explicit(&g_tr_sig, 1, memory_order_relaxed);
    }
}

/*
 * sw_pick_next: Single-consumer Vyukov MPSC pop.
 *
 * Only called by the owning scheduler thread — no locking needed.
 * Checks each priority level in order (max → low).
 */
sw_process_t *sw_pick_next(sw_scheduler_t *sched) {
    sw_runq_t *rq = &sched->runq;

    for (int prio = 0; prio < SW_PRIO_NUM; prio++) {
        sw_process_t *head = rq->heads[prio];
        sw_process_t *next = atomic_load_explicit(&head->rq_next,
                                                    memory_order_acquire);

        /* Skip past stub node if it's at the head */
        if (head == &rq->stubs[prio]) {
            if (!next) continue;  /* Empty queue at this priority */
            rq->heads[prio] = next;
            head = next;
            next = atomic_load_explicit(&head->rq_next, memory_order_acquire);
        }

        /* If there's a next node, we can safely dequeue head */
        if (next) {
            rq->heads[prio] = next;
            return head;
        }

        /* Head is the last element. Check if a push is in-progress.
         * A producer may have done atomic_exchange on tail but not yet
         * stored prev->rq_next. Spin until the link appears. */
        sw_process_t *tail = atomic_load_explicit(&rq->tails[prio],
                                                    memory_order_acquire);
        if (head != tail) {
            /* Push in progress — spin for the link to land */
            for (int spin = 0; spin < 100; spin++) {
                next = atomic_load_explicit(&head->rq_next,
                                             memory_order_acquire);
                if (next) {
                    rq->heads[prio] = next;
                    return head;
                }
                /* Pause hint — reduces pipeline stalls on spin */
#ifdef __aarch64__
                __asm__ volatile("yield");
#else
                __asm__ volatile("pause");
#endif
            }
            continue;  /* Still not ready after 100 spins — try later */
        }

        /* Queue has exactly one element. Re-insert stub to make the queue
         * functional for future pushes, then try to dequeue head. */
        atomic_store_explicit(&rq->stubs[prio].rq_next, NULL,
                               memory_order_relaxed);
        sw_process_t *prev = atomic_exchange_explicit(&rq->tails[prio],
                                                       &rq->stubs[prio],
                                                       memory_order_acq_rel);
        atomic_store_explicit(&prev->rq_next, &rq->stubs[prio],
                               memory_order_release);

        /* Now check if the link from head to stub is visible */
        next = atomic_load_explicit(&head->rq_next, memory_order_acquire);
        if (next) {
            rq->heads[prio] = next;
            return head;
        }
    }

    return NULL;
}

/*
 * Work stealing via global overflow queue.
 * When a scheduler has no local work, it tries to steal from the shared queue.
 * Mutex-protected since this is the cold path (idle schedulers only).
 */
sw_process_t *sw_steal_work(sw_scheduler_t *sched) {
    if (!g_swarm) return NULL;
    sched->steal_attempts++;

    pthread_mutex_lock(&g_swarm->overflow_rq.lock);
    sw_process_t *proc = g_swarm->overflow_rq.head;
    if (proc) {
        g_swarm->overflow_rq.head = (sw_process_t *)atomic_load_explicit(
            &proc->rq_next, memory_order_relaxed);
        if (!g_swarm->overflow_rq.head)
            g_swarm->overflow_rq.tail = NULL;
        g_swarm->overflow_rq.count--;
        atomic_store_explicit(&proc->rq_next, NULL, memory_order_relaxed);
    }
    pthread_mutex_unlock(&g_swarm->overflow_rq.lock);

    return proc;
}

/*
 * Push a process to the global overflow queue.
 * Called when spawning and the target scheduler's queue is heavily loaded.
 */
static void overflow_rq_push(sw_process_t *proc) {
    if (!g_swarm) return;
    atomic_store_explicit(&proc->rq_next, NULL, memory_order_relaxed);

    pthread_mutex_lock(&g_swarm->overflow_rq.lock);
    if (g_swarm->overflow_rq.tail) {
        atomic_store_explicit(&g_swarm->overflow_rq.tail->rq_next, proc, memory_order_relaxed);
        g_swarm->overflow_rq.tail = proc;
    } else {
        g_swarm->overflow_rq.head = proc;
        g_swarm->overflow_rq.tail = proc;
    }
    g_swarm->overflow_rq.count++;
    pthread_mutex_unlock(&g_swarm->overflow_rq.lock);
}

/* ============================================================================
 * SAFE SWAP-IN
 * ============================================================================
 *
 * Closes the high-process-count crash (R2-#4): scheduler X swapping into
 * P concurrently with the slot being reused for Q by another scheduler.
 *
 * Earlier attempts (R3-C 1-slot ring, R4-B 64-slot ring) just delayed the
 * slot return — heuristic, not deterministic, and R4-B actually regressed
 * 40k/50k thresholds by spreading allocator pressure.
 *
 * R5 approach: under per-slot ctx_lock, (1) verify the generation we saw
 * at pick time still matches and (2) copy ctx to a stack-local. After
 * the unlock, the asm reads from the local copy — process_init_arena on
 * any other thread can run freely; it can't tear our snapshot. Returns
 * 0 if the swap happened, -1 if the slot was reused before we could swap
 * (caller treats this as "process disappeared, pick something else"). */
static int sw_safe_swap_into(sw_process_t *from, sw_process_t *to,
                             uint64_t expected_gen) {
    sw_context_t local_ctx;
    sw_spin_lock(&to->ctx_lock);
    uint64_t actual = atomic_load_explicit(&to->generation, memory_order_acquire);
    if (actual != expected_gen) {
        sw_spin_unlock(&to->ctx_lock);
        return -1;
    }
    local_ctx = to->ctx;
    sw_spin_unlock(&to->ctx_lock);
    sw_context_swap_from_copy(from, &local_ctx);
    return 0;
}

/* ============================================================================
 * SCHEDULER THREAD
 * ============================================================================ */

/*
 * root_exit_check: If the ROOT entry process (pid 1 — the one the
 * codegen `main()` spawns to run the user's `main()`) terminates
 * ABNORMALLY (a panic / uncaught crash, reason != 0), force the whole
 * program to exit nonzero.
 *
 * Why this exists: the codegen `_main_entry` only sets `_sw_done_flag`
 * (which wakes the blocked C main thread) AFTER the user's main()
 * RETURNS. On a panic, sw_process_panic is NORETURN — it swaps back to
 * the scheduler, which tears this process down via process_exit, and the
 * entry function never returns. So _sw_done_flag is never set and the C
 * main thread waits on pthread_cond_wait forever — the binary hangs
 * instead of exiting 1 (contradicting SW_LANGUAGE.md "then exits with
 * code 1"). The exit(1) at the tail of _sw_runtime_panic is dead code
 * for the in-process case (the panic helper never returns from
 * sw_process_panic).
 *
 * The fix is scoped tightly to the root process so process isolation is
 * preserved: a SPAWNED child (pid > 1) panicking still flows through the
 * normal link / monitor / trap_exit / supervisor machinery in
 * process_exit, and its parent survives. Only pid 1 going down abnormally
 * means "the program's main crashed", which is a fatal whole-binary
 * condition. A normal root exit (reason == 0) is left alone — the C main
 * thread is already being woken by _sw_done_flag in that case.
 */
static void root_exit_check(sw_process_t *proc, int reason) {
    if (proc && proc->pid == 1 && reason != 0) {
        /* Trace + panic banner were already printed and flushed by the
         * panic helper before it swapped back here. Exit nonzero so the
         * program reports failure instead of hanging. */
        fflush(stdout);
        fflush(stderr);
        exit(1);
    }
}

static void scheduler_loop(sw_scheduler_t *sched) {
    tls_scheduler = sched;
    _sw_gen = &_sw_gen_fallback;  /* valid before the first switch-in sets it */

    while (!sched->should_exit && g_swarm && g_swarm->running) {
        sched->loop_iters++;

        /* Fire expired timers (any scheduler can fire any timer) */
        fire_timers();

        sw_process_t *proc = sw_pick_next(sched);

        if (!proc) {
            proc = sw_steal_work(sched);
        }

        /* Adaptive spin before parking (O4, Round-7: cross-scheduler
         * ping-pong paid a full mutex+condvar+futex wake per message —
         * ~53us/round-trip vs ~1us same-scheduler, because each send
         * found the peer's scheduler PARKED). Spin-poll local + steal
         * for a bounded window first: while spinning, rq->idle stays 0,
         * so the producer's sw_add_to_runq pays NOTHING (no lock, no
         * signal) and the consumer picks the work up within ~ns.
         * Budget is per-idle-transition, env-tunable: SW_SPIN_US
         * (default 30, 0 disables, capped 1000); the 0.5ms park below is
         * the safety net. Default ON since the deadlock that briefly
         * forced it opt-in was root-caused to the Dekker StoreLoad bug
         * in the receive waiting-flag handshake (see sw_receive_any) and
         * fixed with seq_cst — 0/200 wedge-hunt runs post-fix vs ~15%
         * incidence before. Regression gate:
         * tests/stress/spin_wedge_hunt.sh. Measured win: cross-scheduler
         * ping-pong 58.4 -> 4.5us/rt. */
        if (!proc) {
            static int spin_iters = -1;
            if (spin_iters < 0) {
                const char *e = getenv("SW_SPIN_US");
                int us = e ? atoi(e) : 30;
                if (us < 0) us = 0;
                if (us > 1000) us = 1000;
                /* ~25 pause-loop iterations per us on contemporary cores
                 * (pause ~25-40ns + two acquire loads). Coarse is fine. */
                spin_iters = us * 25;
            }
            for (int i = 0; i < spin_iters && !sched->should_exit; i++) {
                proc = sw_pick_next(sched);
                if (proc) break;
                if ((i & 63) == 63) {   /* steal probe every ~64 spins */
                    proc = sw_steal_work(sched);
                    if (proc) break;
                }
#ifdef __aarch64__
                __asm__ volatile("yield");
#else
                __asm__ volatile("pause");
#endif
            }
            if (proc && g_sched_trace >= 1)
                atomic_fetch_add_explicit(&g_tr_spin_hit, 1, memory_order_relaxed);
        }

        if (proc) {
            /* Sample the generation BEFORE any side effect. If another
             * scheduler reuses this slot between pick and swap, the
             * generation will have advanced and sw_safe_swap_into
             * returns -1. */
            uint64_t expected_gen = atomic_load_explicit(&proc->generation,
                                                         memory_order_acquire);

            /* Check if this process was killed by an exit signal */
            if (proc->kill_flag) {
                atomic_store_explicit(&proc->state, SW_PROC_EXITING, memory_order_relaxed);
                process_exit(proc, proc->exit_reason);
                /* Force whole-binary exit if the ROOT process died
                 * abnormally — checked before process_destroy recycles
                 * the slot (and may not return). */
                root_exit_check(proc, proc->exit_reason);
                process_destroy(proc);
                continue;
            }

            sched->procs_run++;
            atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
            proc->scheduler = sched;
            proc->fcalls = SWARM_CONTEXT_REDS;
            sched->current = proc;
            tls_current = proc;
            /* Point generated line/file/trace state at THIS process (fallback if
             * gen_exec wasn't allocated, e.g. OOM — diagnostic-only degradation). */
            _sw_gen = proc->gen_exec ? proc->gen_exec : &_sw_gen_fallback;

            /* Context switch to process (runs on process's own stack).
             * sw_safe_swap_into copies ctx under proc->ctx_lock so the
             * asm reads a stable snapshot. */
            if (sw_safe_swap_into(&sched->sched_proc, proc, expected_gen) < 0) {
                /* Slot was reused between pick and swap — just loop. */
                tls_current = NULL;
                _sw_gen = &_sw_gen_fallback;
                sched->current = NULL;
                continue;
            }

            /* Process yielded, blocked on receive, or finished */
            tls_current = NULL;
            _sw_gen = &_sw_gen_fallback;
            sched->current = NULL;

            if (proc->state == SW_PROC_EXITING) {
                /* Process finished or killed — clean up */
                process_exit(proc, proc->exit_reason);
                /* Force whole-binary exit if the ROOT process (pid 1)
                 * died abnormally (panic). A normal root exit (reason 0)
                 * and any non-root exit are left untouched, preserving
                 * link/monitor/trap_exit isolation. Checked before
                 * process_destroy recycles the slot. */
                root_exit_check(proc, proc->exit_reason);
                process_destroy(proc);
            } else if (proc->state == SW_PROC_RUNNABLE) {
                /* Process yielded — put back on run queue */
                sw_add_to_runq(&sched->runq, proc);
            }
            /* If WAITING: don't re-enqueue. sw_send will re-enqueue when message arrives. */

        } else {
            sched->idle_waits++;
            /* Mark as idle and sleep until woken by a producer.
             * The idle flag is checked by sw_add_to_runq — if set,
             * it signals the condvar to wake us.
             *
             * LOST-WAKEUP GUARD (standard sleeper protocol): a producer
             * that enqueues between our last failed pick and the idle=1
             * store sees idle==0 and SKIPS the signal — the work then
             * sits for the full 0.5ms timeout. A serial message chain
             * can phase-lock into that miss on EVERY hop: observed as a
             * gc-slope message probe degrading from ~100ms to 25+
             * MINUTES (each hop eating a 0.5ms park). Publish idle=1
             * first, then RE-POLL once; only park if still empty. The
             * producer now either sees idle=1 (and signals) or its
             * enqueue happens-before our re-poll (and we find it). */
            sw_runq_t *rq = &sched->runq;
            pthread_mutex_lock(&rq->idle_lock);
            atomic_store_explicit(&rq->idle, 1, memory_order_release);
            sw_process_t *late = sw_pick_next(sched);
            if (!late) late = sw_steal_work(sched);
            if (late) {
                atomic_store_explicit(&rq->idle, 0, memory_order_release);
                pthread_mutex_unlock(&rq->idle_lock);
                if (g_sched_trace >= 1)
                    atomic_fetch_add_explicit(&g_tr_guard_hit, 1, memory_order_relaxed);
                /* Hand it back to our queue and loop — the next pick
                 * runs it through the normal path immediately. */
                sw_add_to_runq(rq, late);
                continue;
            }
            if (g_sched_trace >= 1)
                atomic_fetch_add_explicit(&g_tr_park, 1, memory_order_relaxed);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 500000;  /* +0.5ms (safety net; signals do the work) */
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            int _twrc = pthread_cond_timedwait(&rq->idle_cond, &rq->idle_lock, &ts);
            if (g_sched_trace >= 1)
                atomic_fetch_add_explicit(_twrc == 0 ? &g_tr_park_signal : &g_tr_park_timeout,
                                          1, memory_order_relaxed);
            atomic_store_explicit(&rq->idle, 0, memory_order_release);
            pthread_mutex_unlock(&rq->idle_lock);
        }
    }
}

static void _sw_install_altstack(void);   /* defined with the crash handler below */

#ifdef SW_ALLOC_FAULT
/* Phase 2.5 allocation-failure injection. Armed by SW_FAIL_ALLOC_AT=N
 * (read once): the Nth instrumented allocation (val_alloc global-heap
 * fallback + every varena chunk_new) returns failure exactly once, so a
 * sweep over N drives a fault through each ownership-transfer site and
 * asserts (under ASAN) the cleanup neither leaks nor double-frees. Atomic
 * because allocations happen on every scheduler thread. Compiled only
 * under -DSW_ALLOC_FAULT — not in any shipping build. */
static _Atomic long g_alloc_fault_count = 0;
static long g_alloc_fault_at = -2;   /* -2 = unread, -1 = disabled */
int sw_alloc_fault_tick(void) {
    if (g_alloc_fault_at == -2) {
        const char *e = getenv("SW_FAIL_ALLOC_AT");
        g_alloc_fault_at = e ? atol(e) : -1;
    }
    if (g_alloc_fault_at < 0) return 0;
    long n = atomic_fetch_add_explicit(&g_alloc_fault_count, 1, memory_order_relaxed) + 1;
    return n == g_alloc_fault_at;
}
#endif

static void *scheduler_main(void *arg) {
    sw_scheduler_t *sched = (sw_scheduler_t *)arg;
    _sw_install_altstack();   /* fiber-stack overflows fault on THIS thread */
    sched->active = 1;
    scheduler_loop(sched);
    return NULL;
}

/* ============================================================================
 * CRASH HANDLER — print backtrace on SIGSEGV/SIGBUS/SIGABRT
 * ============================================================================ */
#include <execinfo.h>

/* Per-thread alternate signal stack. A fiber-stack OVERFLOW faults on the
 * guard page with the stack pointer already at the cliff edge — the handler
 * itself then faults trying to push a frame, and the process dies with NO
 * output at all (observed: deep mutual recursion exited silently). SA_ONSTACK
 * + a per-thread sigaltstack lets the handler run on safe ground. Installed
 * by each scheduler thread and the thread that calls sw_init. 64KB static
 * per thread (SIGSTKSZ stopped being a compile-time constant in glibc 2.34). */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define SW_UNDER_ASAN 1
#  endif
#endif
#if !defined(SW_UNDER_ASAN) && defined(__SANITIZE_ADDRESS__)
#  define SW_UNDER_ASAN 1
#endif

static void _sw_install_altstack(void) {
#ifdef SW_UNDER_ASAN
    /* ASAN installs (and at thread exit munmaps) its OWN per-thread
     * alternate signal stack; replacing it with our static buffer makes
     * its UnsetAlternateSignalStack CHECK-fail ("unable to unmmap").
     * Under ASAN the crash handler runs on ASAN's altstack anyway. */
    return;
#else
    static __thread char altstack_mem[64 * 1024];
    stack_t ss;
    ss.ss_sp = altstack_mem;
    ss.ss_size = sizeof(altstack_mem);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
#endif
}

static void _sw_crash_handler(int sig, siginfo_t *info, void *ctx) {
    (void)ctx;
    const char *signame = (sig == SIGSEGV) ? "SIGSEGV" :
                          (sig == SIGBUS)  ? "SIGBUS"  :
                          (sig == SIGABRT) ? "SIGABRT" : "SIGNAL";

    /* Use write() not printf() — async-signal-safe */
    char msg[512];
    int len;

    /* Stack overflow detection: a fault inside (or within a page below) the
     * RUNNING process's guard page means its 128KB fiber stack overflowed —
     * almost always deep non-self recursion (mutual tail calls are not
     * TCO'd; self-tail-calls compile to a flat loop). Say so, instead of
     * leaving a bare SIGSEGV to be mistaken for a runtime bug. */
    sw_process_t *cur = tls_current;
    if (sig == SIGSEGV && cur && cur->stack_mem && info->si_addr) {
        uintptr_t fault = (uintptr_t)info->si_addr;
        uintptr_t guard_top = (uintptr_t)cur->stack_mem;       /* guard page is just below */
        uintptr_t guard_bot = guard_top - 8192;                /* guard page + one page slack */
        if (fault < guard_top && fault >= guard_bot) {
            len = snprintf(msg, sizeof(msg),
                "\n\033[1;31mpanic\033[0m: stack overflow in process #%llu (128KB fiber stack)\n"
                "  likely cause: deep NON-SELF recursion — mutual tail calls are not\n"
                "  TCO'd (self-tail-calls are). Keep the loop in one function, or see\n"
                "  docs/notes/KNOWN_ISSUES.md.\n",
                (unsigned long long)cur->pid);
            write(STDERR_FILENO, msg, len);
            signal(sig, SIG_DFL);
            raise(sig);
            return;
        }
    }

    len = snprintf(msg, sizeof(msg),
        "\n\033[31m[SwarmRT] CRASH: %s at address %p\033[0m\n"
        "Backtrace:\n",
        signame, info->si_addr);
    write(STDERR_FILENO, msg, len);

    void *frames[32];
    int nframes = backtrace(frames, 32);
    backtrace_symbols_fd(frames, nframes, STDERR_FILENO);

    write(STDERR_FILENO, "\n", 1);

    /* Re-raise to get the default core dump */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

int sw_init(const char *name, uint32_t num_schedulers) {
    pthread_mutex_lock(&g_init_lock);

    if (g_swarm) {
        pthread_mutex_unlock(&g_init_lock);
        return -1;
    }

    g_swarm = (sw_swarm_t *)calloc(1, sizeof(sw_swarm_t));
    if (!g_swarm) {
        pthread_mutex_unlock(&g_init_lock);
        return -1;
    }

    strncpy(g_swarm->name, name, 31);
    g_swarm->num_schedulers = num_schedulers;
    g_swarm->running = 1;
    sched_trace_maybe_start();   /* BEFORE scheduler threads exist — the flag
                                  * is then ordered by pthread_create (TSan-
                                  * clean); scheds read it on every enqueue. */

    /* Initialize arena — single mmap, covers everything.
     *
     * Default ceiling is SWARM_MAX_PROCESSES (100k slots, ~244MB virtual
     * mmap + ~8MB of free-list writes). For CLI binaries that never spawn
     * more than a handful of processes, that pre-allocation is the
     * dominant boot cost (~20ms on macOS arm64). SW_MAX_PROCS lets a
     * caller dial the ceiling down for fast-start programs.
     *
     * Safe minimum is ~16 — enough room for the supervisor tree most
     * .sw programs spawn at startup. swarm-code peaks around 50; agent
     * harnesses doing parallel subagents may want 1024+. Headroom is
     * cheap (virtual mmap only commits pages touched) so leave the
     * default high for backward compat. */
    uint32_t max_procs = SWARM_MAX_PROCESSES;
    const char *max_procs_env = getenv("SW_MAX_PROCS");
    if (max_procs_env && *max_procs_env) {
        long n = strtol(max_procs_env, NULL, 10);
        if (n >= 16 && n <= (long)SWARM_MAX_PROCESSES) max_procs = (uint32_t)n;
    }

    /* Mailbox depth cap — SW_MAILBOX_MAX overrides the 1M default, 0 disables.
     * Parsed here, before the scheduler threads start, so the plain global is
     * safely published by pthread_create. Negative/garbage input is ignored. */
    const char *mb_max_env = getenv("SW_MAILBOX_MAX");
    if (mb_max_env && *mb_max_env) {
        long long n = strtoll(mb_max_env, NULL, 10);
        if (n >= 0) g_mailbox_max = (int64_t)n;
    }

    /* Per-process memory quota — SW_PROC_MEM_MAX (bytes; 0/unset = unlimited).
     * Same pre-scheduler publication as the mailbox cap. Negative/garbage
     * input is ignored (strtoll so "-1" doesn't wrap into a huge quota). */
    const char *pm_max_env = getenv("SW_PROC_MEM_MAX");
    if (pm_max_env && *pm_max_env) {
        long long n = strtoll(pm_max_env, NULL, 10);
        if (n > 0) g_proc_mem_max = (size_t)n;
    }

    /* Max local message size — SW_MSG_MAX_BYTES (bytes; 0/unset = unlimited).
     * Same pre-scheduler publication; negative/garbage input is ignored. */
    const char *msg_max_env = getenv("SW_MSG_MAX_BYTES");
    if (msg_max_env && *msg_max_env) {
        long long n = strtoll(msg_max_env, NULL, 10);
        if (n > 0) g_msg_max_bytes = (size_t)n;
    }

    /* Reset the graceful-shutdown flags so an embedder that re-inits after a
     * prior sw_shutdown starts clean. No signal can have arrived yet — the
     * SIGTERM/SIGINT handler is installed only from the entry path AFTER
     * sw_init returns (sw_install_shutdown_signals). */
    atomic_store_explicit(&g_sw_draining, 0, memory_order_relaxed);
    atomic_store_explicit(&g_sw_shutdown_requested, 0, memory_order_relaxed);

    if (sw_arena_init(&g_swarm->arena, max_procs) != 0) {
        free(g_swarm);
        g_swarm = NULL;
        pthread_mutex_unlock(&g_init_lock);
        return -1;
    }

    /* Initialize process registry */
    g_swarm->registry.num_buckets = SW_REGISTRY_BUCKETS;
    g_swarm->registry.buckets = (sw_reg_entry_t **)calloc(SW_REGISTRY_BUCKETS,
                                                            sizeof(sw_reg_entry_t *));
    pthread_rwlock_init(&g_swarm->registry.lock, NULL);

    /* Initialize timer list */
    g_swarm->timers.head = NULL;
    pthread_mutex_init(&g_swarm->timers.lock, NULL);

    /* Initialize link/monitor lock and counters */
    pthread_mutex_init(&g_swarm->link_lock, NULL);
    atomic_store(&g_swarm->next_monitor_ref, 1);
    atomic_store(&g_swarm->next_timer_ref, 1);

    /* Initialize global overflow run queue for work stealing */
    g_swarm->overflow_rq.head = NULL;
    g_swarm->overflow_rq.tail = NULL;
    g_swarm->overflow_rq.count = 0;
    pthread_mutex_init(&g_swarm->overflow_rq.lock, NULL);

    /* Create schedulers (still malloc'd — only a few, not on hot path) */
    g_swarm->schedulers = (sw_scheduler_t **)calloc(num_schedulers,
                                                      sizeof(sw_scheduler_t *));

    for (uint32_t i = 0; i < num_schedulers; i++) {
        sw_scheduler_t *sched = (sw_scheduler_t *)calloc(1, sizeof(sw_scheduler_t));
        if (!sched) continue;

        sched->id = i;
        sched->swarm = g_swarm;

        /* Initialize MPSC queues — stub is both head and tail */
        for (int p = 0; p < SW_PRIO_NUM; p++) {
            atomic_store(&sched->runq.stubs[p].rq_next, NULL);
            sched->runq.heads[p] = &sched->runq.stubs[p];
            atomic_store(&sched->runq.tails[p], &sched->runq.stubs[p]);
        }
        atomic_store(&sched->runq.idle, 0);
        pthread_mutex_init(&sched->runq.idle_lock, NULL);
        pthread_cond_init(&sched->runq.idle_cond, NULL);

        g_swarm->schedulers[i] = sched;
        pthread_create(&sched->thread, NULL, scheduler_main, sched);
    }

    pthread_mutex_unlock(&g_init_lock);

    /* Wait for schedulers to flip their `active` flag before returning.
     * Was usleep(10000) — a fixed 10ms padding that dominated boot time
     * for short-lived CLI binaries. Spin-polling is ~100x faster on the
     * common case (threads come up in <100us on modern OSes) while
     * preserving the invariant that the first sw_spawn after sw_init
     * sees ready schedulers. Falls back to the old 10ms cap if anything
     * is genuinely stuck. */
    {
        int spins = 0;
        int max_spins = 200;  /* 200 * 50us = 10ms ceiling */
        int all_ready = 0;
        while (!all_ready && spins < max_spins) {
            all_ready = 1;
            for (uint32_t i = 0; i < num_schedulers; i++) {
                if (!g_swarm->schedulers[i] || !g_swarm->schedulers[i]->active) {
                    all_ready = 0;
                    break;
                }
            }
            if (!all_ready) {
                usleep(50);
                spins++;
            }
        }
    }

    /* Startup banner — diagnostics, not program output, so it goes to
     * stderr. That keeps stdout clean for programs whose output is
     * piped or captured (e.g. a CLI answering `--version`). Silence it
     * entirely with SW_QUIET=1 or SW_RUNTIME_QUIET=1. The latter is the
     * runtime-only knob a headless agent sets in the *built binary's*
     * environment so the two "[SwarmRT] Arena initialized…" lines never
     * leak into a captured stream, without having to also be set at
     * compile time. */
    if (!getenv("SW_QUIET") && !getenv("SW_RUNTIME_QUIET")) {
        fprintf(stderr, "[SwarmRT] Arena initialized: %zu MB mmap, %u proc slots, %u heap blocks\n",
               g_swarm->arena.size / (1024 * 1024),
               g_swarm->arena.proc_capacity,
               g_swarm->arena.block_count);
        fprintf(stderr, "[SwarmRT] Swarm '%s' initialized with %d scheduler(s)\n",
               name, num_schedulers);
        fflush(stderr);
    }

    /* Install crash handler so segfaults produce a backtrace instead of
     * a bare "segmentation fault" message.  macOS provides backtrace()
     * in <execinfo.h> which gives us symbol names + offsets. */
    {
        struct sigaction crash_sa;
        memset(&crash_sa, 0, sizeof(crash_sa));
        crash_sa.sa_sigaction = _sw_crash_handler;
        /* SA_ONSTACK: run on the per-thread sigaltstack. On a fiber-stack
         * overflow the faulting stack has no headroom — without this the
         * handler itself faulted and the process died with no output. */
        crash_sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
        sigaction(SIGSEGV, &crash_sa, NULL);
        sigaction(SIGBUS,  &crash_sa, NULL);
        sigaction(SIGABRT, &crash_sa, NULL);
        _sw_install_altstack();   /* this thread; schedulers install their own */
    }

    /* Start deadlock watchdog unless SW_DEADLOCK_DETECT=0. */
    {
        const char *dd = getenv("SW_DEADLOCK_DETECT");
        g_watchdog_enabled = !(dd && dd[0] == '0' && dd[1] == '\0');
        if (g_watchdog_enabled) {
            g_watchdog_stop = 0;
            pthread_create(&g_watchdog_thread, NULL, watchdog_thread_fn, NULL);
        }
    }

    return 0;
}

void sw_shutdown(int swarm_id) {
    (void)swarm_id;

    if (!g_swarm) return;

    g_swarm->running = 0;

    /* Stop deadlock watchdog (if running) before tearing down the arena.
     * Signal under the lock so the join returns immediately instead of
     * after the tail of a sleep interval (was ~50-100ms on every exit). */
    if (g_watchdog_enabled) {
        pthread_mutex_lock(&g_watchdog_lock);
        g_watchdog_stop = 1;
        pthread_cond_signal(&g_watchdog_cond);
        pthread_mutex_unlock(&g_watchdog_lock);
        pthread_join(g_watchdog_thread, NULL);
        g_watchdog_enabled = 0;
    }

    /* Stop all schedulers */
    for (uint32_t i = 0; i < g_swarm->num_schedulers; i++) {
        sw_scheduler_t *sched = g_swarm->schedulers[i];
        sched->should_exit = 1;
        /* Wake scheduler if idle */
        pthread_mutex_lock(&sched->runq.idle_lock);
        pthread_cond_signal(&sched->runq.idle_cond);
        pthread_mutex_unlock(&sched->runq.idle_lock);
        pthread_join(sched->thread, NULL);
        pthread_mutex_destroy(&sched->runq.idle_lock);
        pthread_cond_destroy(&sched->runq.idle_cond);
        free(sched);
    }

    /* Clean up registry */
    if (g_swarm->registry.buckets) {
        for (uint32_t i = 0; i < g_swarm->registry.num_buckets; i++) {
            sw_reg_entry_t *e = g_swarm->registry.buckets[i];
            while (e) {
                sw_reg_entry_t *next = e->next;
                free(e);
                e = next;
            }
        }
        free(g_swarm->registry.buckets);
        pthread_rwlock_destroy(&g_swarm->registry.lock);
    }

    /* Clean up timers */
    {
        sw_timer_t *t = g_swarm->timers.head;
        while (t) {
            sw_timer_t *next = t->next;
            free(t->msg);
            free(t);
            t = next;
        }
        pthread_mutex_destroy(&g_swarm->timers.lock);
    }

    pthread_mutex_destroy(&g_swarm->link_lock);
    pthread_mutex_destroy(&g_swarm->overflow_rq.lock);

    /* Free process stacks (mmap'd with guard page) */
    if (g_swarm->arena.proc_slab) {
#ifdef _WIN32
        long page_size = 4096;
#else
        long page_size = sysconf(_SC_PAGESIZE);
#endif
        sw_process_t *slab = (sw_process_t *)g_swarm->arena.proc_slab;
        for (uint32_t i = 0; i < g_swarm->arena.proc_capacity; i++) {
            if (slab[i].stack_mem) {
                /* stack_mem points past the guard page — subtract to get mmap base */
                void *base = (uint8_t *)slab[i].stack_mem - page_size;
                size_t total = slab[i].stack_size + page_size;
                total = (total + page_size - 1) & ~(page_size - 1);
#ifdef _WIN32
                VirtualFree(base, 0, MEM_RELEASE);
#else
                munmap(base, total);
#endif
                slab[i].stack_mem = NULL;
            }
            /* Per-process generated exec state — kept with the slot across
             * lifetimes (like stack_mem), so freed here at slab teardown. */
            if (slab[i].gen_exec) {
                free(slab[i].gen_exec);
                slab[i].gen_exec = NULL;
            }
        }
    }

    /* Clean up arena */
    if (g_swarm->arena.base) {
#ifdef _WIN32
        VirtualFree(g_swarm->arena.base, 0, MEM_RELEASE);
#else
        munmap(g_swarm->arena.base, g_swarm->arena.size);
#endif
    }

    free(g_swarm->schedulers);
    free(g_swarm);
    g_swarm = NULL;
}

sw_process_t *sw_spawn(void (*entry)(void*), void *arg) {
    return sw_spawn_opts(entry, arg, SW_PRIO_NORMAL);
}

/*
 * sw_spawn_opts: ZERO-SYSCALL process spawn.
 *
 * Hot path:
 * 1. atomic_fetch_add for PID          (~5ns)
 * 2. arena_pop for process slot        (~10-20ns, lock-free CAS)
 * 3. arena_pop for heap block          (~10-20ns, lock-free CAS)
 * 4. Field initialization              (~100ns, mostly memset-like)
 * 5. pthread_mutex_init for mailbox    (~200ns)
 * 6. Add to run queue                  (~100ns with mutex)
 *
 * Total: ~500ns. No mmap, no malloc, no calloc.
 */
sw_process_t *sw_spawn_opts(void (*entry)(void*), void *arg, sw_priority_t prio) {
    if (!g_swarm) return NULL;

    sw_arena_t *arena = &g_swarm->arena;

    /* 1. Pick the target scheduler.
     *
     * sw_spawn_link sets tls_spawn_override to a specific scheduler
     * (the non-parent one it picked to avoid cooperative-deadlock
     * with a blocking parent). Honour that pin when present —
     * previously sw_spawn_opts always used the global round-robin
     * counter and ignored the hint, so spawn-and-wait patterns like
     * `spawn_link + usleep(50ms)` would routinely land one child on
     * the parent's scheduler, where the child couldn't run until
     * usleep returned and the pg tests in phase5 missed members.
     *
     * Without a pin, round-robin — originally this preferred the
     * current scheduler for zero cross-thread overhead, but that
     * starves background processes when the parent blocks on a C
     * syscall (popen fread, fgetc); the child would be queued on the
     * same pinned thread and never run. Round-robin distributes across
     * all schedulers so heartbeats and watchers keep ticking while
     * main is busy with an HTTP call. */
    uint32_t sched_id;
    if (tls_spawn_override) {
        sched_id = tls_spawn_override->id;
    } else {
        sched_id = __sync_fetch_and_add(&g_swarm->next_sched, 1)
                   % g_swarm->num_schedulers;
    }
    uint32_t part_id = sched_id;
    if (part_id >= arena->num_partitions) part_id = 0;
    sw_arena_partition_t *part = &arena->partitions[part_id];

    /* 2. Pop slot + block from this partition's free list */
    sw_spin_lock(&part->lock);
    int32_t slot = part_pop_pid(part);
    int32_t block_idx = (slot >= 0) ? part_pop_block(part) : -1;
    sw_spin_unlock(&part->lock);

    /* 3. If empty, steal from other partitions */
    if (slot < 0 || block_idx < 0) {
        if (slot >= 0 && block_idx < 0) {
            /* Got slot but no block — push slot back */
            sw_spin_lock(&part->lock);
            part_push_pid(part, (uint32_t)slot);
            sw_spin_unlock(&part->lock);
            slot = -1;
        }
        for (uint32_t i = 0; i < arena->num_partitions && (slot < 0 || block_idx < 0); i++) {
            if (i == part_id) continue;
            sw_arena_partition_t *victim = &arena->partitions[i];
            /* Lock ordering: lower index first to prevent deadlock */
            sw_arena_partition_t *first = (part_id < i) ? part : victim;
            sw_arena_partition_t *second = (part_id < i) ? victim : part;
            sw_spin_lock(&first->lock);
            sw_spin_lock(&second->lock);
            if (slot < 0) part_steal_pids(part, victim, SW_STEAL_BATCH);
            if (block_idx < 0) part_steal_blocks(part, victim, SW_STEAL_BATCH);
            slot = part_pop_pid(part);
            block_idx = (slot >= 0) ? part_pop_block(part) : -1;
            sw_spin_unlock(&second->lock);
            sw_spin_unlock(&first->lock);
        }
        if (slot < 0 || block_idx < 0) return NULL;
    }

    /* 3. Get process pointer from slab (direct index, no hash lookup) */
    sw_process_t *proc = &((sw_process_t *)arena->proc_slab)[slot];
    proc->arena_slot = (uint32_t)slot;

    /* 4. Initialize process with arena block */
    process_init_arena(proc, (uint32_t)block_idx, entry, arg);

    /* GC v2: record the spawn region (if any) BEFORE the child is runnable, so a
     * pre-trampoline kill still reclaims it in process_destroy. Consumed once. */
    proc->spawn_region = g_pending_spawn_region;
    g_pending_spawn_region = NULL;

    /* Record the teardown hook (if any) BEFORE the child is runnable — same
     * reason: a pre-trampoline kill must still reclaim the spawn arg. (Overrides
     * the NULL set by process_init_arena.) Consumed once. */
    proc->on_destroy = g_pending_on_destroy;
    proc->on_destroy_arg = g_pending_on_destroy_arg;
    g_pending_on_destroy = NULL;
    g_pending_on_destroy_arg = NULL;

    /* 5. Assign PID (monotonic, lock-free) */
    proc->pid = atomic_fetch_add(&arena->next_pid, 1);
    atomic_fetch_add(&g_swarm->total_spawns, 1);

    proc->priority = prio;

    /* 6. Add to run queue (scheduler already chosen for partition) */
    sw_scheduler_t *sched = g_swarm->schedulers[sched_id];
    proc->scheduler = sched;

    sw_add_to_runq(&sched->runq, proc);

    return proc;
}

/* GC v2: spawn a process owning `region`. Threads the region to sw_spawn_opts via
 * a thread-local (consumed before the child is runnable). On failure (NULL proc)
 * the region was not recorded — the caller reclaims it. */
sw_process_t *sw_spawn_owned(void (*entry)(void*), void *arg, struct sw_value_arena *region) {
    g_pending_spawn_region = region;
    sw_process_t *p = sw_spawn_opts(entry, arg, SW_PRIO_NORMAL);
    g_pending_spawn_region = NULL;   /* clear if spawn failed before consuming */
    return p;
}

/* Spawn with a teardown hook recorded on the child BEFORE it is runnable, so a
 * pre-trampoline kill (child killed before its entry fn runs to self-arm) still
 * reclaims `arg` via process_destroy -> on_destroy. dtor(arg) frees the spawn
 * arg. On spawn failure (NULL) nothing was recorded; the caller reclaims `arg`.
 * Race-free vs self-arming in the entry fn: the hook is set before sw_add_to_runq
 * (the child cannot run until then), and vs arm-after-spawn: no window where the
 * child could complete + recycle the slot before the parent writes the hook. */
sw_process_t *sw_spawn_dtor(void (*entry)(void*), void *arg, void (*dtor)(void*)) {
    g_pending_on_destroy = dtor;
    g_pending_on_destroy_arg = arg;
    sw_process_t *p = sw_spawn(entry, arg);
    g_pending_on_destroy = NULL;     /* clear if spawn failed before consuming */
    g_pending_on_destroy_arg = NULL;
    return p;
}

/* sw_spawn_link + a pre-runnable teardown hook (see sw_spawn_dtor). */
sw_process_t *sw_spawn_link_dtor(void (*entry)(void*), void *arg, void (*dtor)(void*)) {
    g_pending_on_destroy = dtor;
    g_pending_on_destroy_arg = arg;
    sw_process_t *p = sw_spawn_link(entry, arg);
    g_pending_on_destroy = NULL;
    g_pending_on_destroy_arg = NULL;
    return p;
}

/* GC v2: read+clear the current process's spawn_region (called by the child
 * trampoline so it can adopt the region and destroy won't double-free it). */
struct sw_value_arena *sw_self_take_spawn_region(void) {
    if (!tls_current) return NULL;
    struct sw_value_arena *r = tls_current->spawn_region;
    tls_current->spawn_region = NULL;
    return r;
}

/*
 * sw_find_by_pid: Linear scan of process slab to find a live process by PID.
 * O(n) but only used for distribution routing (rare path).
 */
sw_process_t *sw_find_by_pid(uint64_t pid) {
    if (!g_swarm || pid == 0) return NULL;
    sw_process_t *slab = (sw_process_t *)g_swarm->arena.proc_slab;
    for (uint32_t i = 0; i < g_swarm->arena.proc_capacity; i++) {
        if (slab[i].pid == pid && slab[i].state != SW_PROC_EXITING && slab[i].entry)
            return &slab[i];
    }
    return NULL;
}

/* sw_find_by_pid_any: like sw_find_by_pid but ignores liveness — returns
 * the slab slot for `pid` even if the process has EXITED, as long as the
 * slot hasn't been recycled to a new pid. Lets DOWN/EXIT message
 * synthesis recover the original sw_process_t* so the delivered pid value
 * compares == the one spawn() returned (which still points at the same
 * slab address). Returns NULL only if the slot was reclaimed. */
sw_process_t *sw_find_by_pid_any(uint64_t pid) {
    if (!g_swarm || pid == 0) return NULL;
    sw_process_t *slab = (sw_process_t *)g_swarm->arena.proc_slab;
    for (uint32_t i = 0; i < g_swarm->arena.proc_capacity; i++) {
        if (slab[i].pid == pid)
            return &slab[i];
    }
    return NULL;
}

/*
 * sw_process_done: Called by the assembly trampoline when entry() returns.
 * Marks process as EXITING and context-swaps back to the scheduler.
 */
void sw_process_done(sw_process_t *proc) {
    atomic_store_explicit(&proc->state, SW_PROC_EXITING, memory_order_relaxed);
    sw_context_swap(proc, &proc->scheduler->sched_proc);
    /* Should never reach here */
}

/*
 * sw_process_panic: Mark the current process as crashed and swap back
 * to the scheduler, which will run process_exit() — that's where
 * link-propagation + monitor-DOWN + arena cleanup live. Used by the
 * codegen panic helpers (hd of empty list, /0, panic(), expect()) in
 * place of the previous exit(1), so a single process going down no
 * longer takes the whole binary with it. Supervision + link + monitor
 * + trap_exit now actually work.
 *
 * `reason` is the exit reason that gets propagated to linked processes
 * (or surfaced as the second element of a `{'EXIT', from, reason}`
 * message if the linked peer has trap_exit set). Convention: -1 for
 * unrecoverable panics; 0 = normal exit (don't use for crashes); other
 * values are user-defined.
 */
void sw_process_panic(sw_process_t *proc, int reason, const char *msg) {
    if (!proc) return;  /* shouldn't happen — caller must check sw_self() */
    proc->exit_reason = reason;
    if (msg) {
        /* Strdup so the buffer outlives the caller's stack frame. The
         * scheduler reads this in process_exit, then process_destroy
         * frees it. */
        free(proc->panic_msg);
        proc->panic_msg = strdup(msg);
    }
    atomic_store_explicit(&proc->state, SW_PROC_EXITING, memory_order_relaxed);
    sw_context_swap(proc, &proc->scheduler->sched_proc);
    /* Should never reach here — scheduler tears down the process. */
}

/* SW_PROC_MEM_MAX enforcement (strong impl; weak no-op lives in
 * swarmrt_varena.c for runtime-less links). Called from the varena GROW and
 * ADOPT cold paths. Fires ONLY when `a` is the CURRENT process's installed
 * arena — see the contract in swarmrt_varena.h for why (mid-copy temp regions
 * grow under g_alloc_target and are metered at adopt instead). Over quota →
 * loud stderr banner naming the quota + pid, then sw_process_panic: the
 * PROCESS dies (links/monitors/supervisors observe it; a supervisor can
 * restart it), the node and every other process survive. NORETURN on the
 * kill path (context-swaps to the scheduler); plain return otherwise. */
void sw_varena_quota_check(sw_value_arena_t *a, size_t need) {
    size_t cap = g_proc_mem_max;
    if (cap == 0 || !a) return;
    sw_process_t *proc = tls_current;
    if (!proc || proc->varena != a) return;
    if (a->total_bytes + need <= cap) return;
    char msg[256];
    snprintf(msg, sizeof(msg),
             "SW_PROC_MEM_MAX exceeded in process #%llu: arena %zu bytes"
             " + %zu requested > quota %zu bytes",
             (unsigned long long)proc->pid, a->total_bytes, need, cap);
    fprintf(stderr,
            "\n\033[1;31mpanic\033[0m: %s — killing process"
            " (node continues; a supervisor can restart it)\n", msg);
    fflush(stderr);
    sw_process_panic(proc, -1, msg);
    /* Not reached: sw_process_panic swaps to the scheduler for teardown. */
}

void sw_yield(void) {
    sw_process_t *proc = tls_current;
    if (!proc) return;

    /* Context swap back to scheduler — scheduler will re-enqueue us */
    atomic_store_explicit(&proc->state, SW_PROC_RUNNABLE, memory_order_relaxed);
    proc->context_switches++;
    sw_context_swap(proc, &proc->scheduler->sched_proc);
    /* Resumed — back on our stack */
}

/*
 * sw_process_kill: Kill a process from outside (supervisor/task shutdown).
 * Sets kill_flag + exit_reason and wakes the process if waiting.
 */
void sw_process_kill(sw_process_t *proc, int reason) {
    if (!proc) return;
    proc->kill_flag = 1;
    proc->exit_reason = reason;
    mailbox_wake(proc);
}

sw_process_t *sw_self(void) {
    return tls_current;
}

/* Report the running process's FIBER stack bounds so the interpreter's
 * stack-near-limit guard measures the right stack. When the tree-walking
 * interpreter runs ON a process fiber (e.g. `swc run`'s root process, or a
 * studio tool fiber), the OS-thread stack bounds reported by
 * pthread_get_stacksize_np() belong to the scheduler thread, not the
 * swapped-in fiber — so the guard mis-fires. Returns 1 and fills *low
 * (lowest usable addr) / *high (top of stack) when on a fiber; returns 0
 * (use OS-thread bounds) otherwise. */
int sw_self_stack_bounds(uintptr_t *low, uintptr_t *high) {
    sw_process_t *p = tls_current;
    if (!p || !p->stack_mem) return 0;
    if (low)  *low  = (uintptr_t)p->stack_mem;
    if (high) *high = (uintptr_t)p->stack_mem + p->stack_size;
    return 1;
}

/* GC v1: the running process's value arena (NULL outside a fiber). The value
 * constructors in swarmrt_lang.c route allocations here. */
sw_value_arena_t *sw_self_varena(void) {
    return tls_current ? tls_current->varena : NULL;
}

/* Per-process generated-error slot (the _sw_error macro derefs this). Returns
 * &proc->gen_error for the running process, else a thread-local fallback (REPL/
 * interpreter / pre-fiber). Per-process so a try/catch can't catch another
 * fiber's error across a blocking-op context switch (which would UAF that
 * fiber's freed error value). */
struct sw_val **sw_self_error_slot(void) {
    static __thread struct sw_val *fallback = NULL;
    return tls_current ? &tls_current->gen_error : &fallback;
}

/* Innermost live compiled try frame (see try_chain in struct sw_process).
 * Thread-local fallback covers the pre-fiber/main-thread path the same way
 * sw_self_error_slot's does. */
void **sw_self_try_chain(void) {
    static __thread void *fallback = NULL;
    return tls_current ? &tls_current->try_chain : &fallback;
}

/* Ownership v2 turn-checkpoint: install a fresh value arena for the current
 * process (the caller copied carry-forward state into `a` and will free the old). */
void sw_set_self_varena(sw_value_arena_t *a) {
    if (tls_current) tls_current->varena = a;
}

uint64_t sw_getpid(void) {
    sw_process_t *proc = tls_current;
    return proc ? proc->pid : 0;
}

/* ============================================================================
 * LOCK-FREE MAILBOX (Vyukov MPSC + process-local private queue)
 * ============================================================================ */

/*
 * mailbox_push: Lock-free LIFO push (CAS-based stack).
 * Multiple senders can push concurrently without locks.
 */
static inline void mailbox_push(sw_mailbox_t *mb, sw_msg_t *m) {
    sw_msg_t *old_head;
    do {
        old_head = atomic_load_explicit(&mb->sig_head, memory_order_relaxed);
        atomic_store_explicit(&m->sig_next, old_head, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(
        &mb->sig_head, &old_head, m,
        memory_order_release, memory_order_relaxed));
}

/*
 * mailbox_drain: Atomically steal signal stack, reverse to FIFO,
 * append to private queue. Single-consumer only.
 */
static void mailbox_drain(sw_mailbox_t *mb) {
    /* Atomic steal: grab entire signal stack */
    sw_msg_t *chain = atomic_exchange_explicit(&mb->sig_head, NULL, memory_order_acquire);
    if (!chain) return;

    /* Reverse LIFO chain to FIFO order */
    sw_msg_t *reversed = NULL;
    while (chain) {
        sw_msg_t *next = (sw_msg_t *)atomic_load_explicit(&chain->sig_next, memory_order_relaxed);
        chain->next = reversed;  /* Reuse private-list 'next' for reversal */
        reversed = chain;
        chain = next;
    }

    /* Append FIFO chain to private doubly-linked list */
    sw_msg_t *m = reversed;
    while (m) {
        sw_msg_t *next_in_chain = m->next;
        m->next = NULL;
        m->prev = mb->priv_tail;
        if (mb->priv_tail)
            mb->priv_tail->next = m;
        else
            mb->priv_head = m;
        mb->priv_tail = m;
        mb->count++;
        m = next_in_chain;
    }
}

/*
 * mailbox_pop_first: Pop first message from private queue.
 * Returns NULL if private queue is empty (caller should drain first).
 * Takes the PROCESS (not the mailbox) so the depth counter (proc->mb_len,
 * kept outside sw_mailbox_t for asm-offset reasons) stays in lockstep
 * with mb->count.
 */
static inline sw_msg_t *mailbox_pop_first(sw_process_t *proc) {
    sw_mailbox_t *mb = &proc->mailbox;
    sw_msg_t *m = mb->priv_head;
    if (!m) return NULL;

    mb->priv_head = m->next;
    if (mb->priv_head)
        mb->priv_head->prev = NULL;
    else
        mb->priv_tail = NULL;
    mb->count--;
    atomic_fetch_sub_explicit(&proc->mb_len, 1, memory_order_relaxed);
    return m;
}

/*
 * mailbox_pop_tagged: Scan private queue for first message with matching tag.
 * Removes and returns it. Non-matching messages stay in place.
 */
static inline sw_msg_t *mailbox_pop_tagged(sw_process_t *proc, uint64_t tag) {
    sw_mailbox_t *mb = &proc->mailbox;
    sw_msg_t *m = mb->priv_head;
    while (m) {
        if (m->tag == tag) {
            /* Remove from doubly-linked list */
            if (m->prev) m->prev->next = m->next;
            else mb->priv_head = m->next;
            if (m->next) m->next->prev = m->prev;
            else mb->priv_tail = m->prev;
            mb->count--;
            atomic_fetch_sub_explicit(&proc->mb_len, 1, memory_order_relaxed);
            return m;
        }
        m = m->next;
    }
    return NULL;
}

/*
 * mailbox_wake: Wake a process if it's waiting on receive.
 * Lock-free: uses atomic exchange on waiting flag.
 *
 * Direct handoff: if we're in a process context (sender is a process),
 * put the woken process on the SENDER's scheduler run queue. This avoids
 * cross-thread condvar signaling (~1-3us on macOS) for the common case
 * of ping-pong messaging between two processes.
 */
static inline void mailbox_wake(sw_process_t *to) {
    /* seq_cst: pairs with the seq_cst waiting-stores + sig_head probe in
     * the receive paths (Dekker — see sw_receive_any). */
    if (atomic_exchange_explicit(&to->mailbox.waiting, 0, memory_order_seq_cst)) {
        trev(TREV_WAKE1, to->pid, 0);
        /* Was waiting — re-enqueue to run queue.
         * Do NOT set state here. The receiver may be in its final drain
         * and could set state=WAITING for context-swap. If we also write
         * state=RUNNABLE, the scheduler's post-swap handler would re-enqueue,
         * causing a double-enqueue. Let the scheduler set RUNNING on dequeue. */
        sw_add_to_runq(&to->scheduler->runq, to);
    } else {
        trev(TREV_WAKE0, to->pid, 0);
    }
}

/* ============================================================================
 * MESSAGE PASSING
 * ============================================================================ */

/*
 * mailbox_admit: depth-cap admission for USER messages (SW_MAILBOX_MAX).
 * Optimistically reserves a slot (fetch_add) and backs out on overflow, so
 * the cap is APPROXIMATE under concurrency — transient overshoot is bounded
 * by the number of concurrent senders, which is exactly the precision a
 * flood guard needs. Returns 1 = admitted (mb_len reserved), 0 = full
 * (reservation rolled back; caller must take the drop path, never enqueue).
 * Runs BEFORE msg_alloc so a rejected send allocates no envelope.
 */
static inline int mailbox_admit(sw_process_t *to) {
    int64_t n = atomic_fetch_add_explicit(&to->mb_len, 1, memory_order_relaxed);
    if (g_mailbox_max && n >= g_mailbox_max) {
        atomic_fetch_sub_explicit(&to->mb_len, 1, memory_order_relaxed);
        return 0;
    }
    return 1;
}

/*
 * mailbox_overflow_drop: LOUD, leak-free, deadlock-free rejection of a send
 * that failed admission. Payload ownership transfers at send (the exact
 * invariant process_destroy already relies on when freeing a dead process's
 * unread queue), so we free it here, mirroring process_destroy's shape:
 *   - VALUE payload (region != NULL): the graph lives in the region —
 *     bulk-free the region, never free(payload).
 *   - RAW payload: plain free (shallow — same as process_destroy; never
 *     _sw_free_global_val, which crashes on non-value structs).
 *   - EXIT/DOWN reason_str: unreachable via the capped producers
 *     (deliver_signal bypasses the cap) — handled anyway, defensively, in
 *     case C callers hand-roll those tags through sw_send_tagged.
 * Then bump the global drop counter, warn (first drop + every 65536th), and
 * CRITICALLY wake the receiver anyway: a spurious wake is harmless (every
 * receive loop re-checks), but a receiver parked while all inbound is being
 * dropped would never drain — livelock. The wake call is load-bearing.
 */
static void mailbox_overflow_drop(sw_process_t *to, uint64_t tag,
                                  void *payload, sw_value_arena_t *region) {
    if (region) {
        sw_varena_free_all(region);
    } else if (payload) {
        if (tag == SW_TAG_EXIT || tag == SW_TAG_DOWN) {
            sw_signal_t *s = (sw_signal_t *)payload;
            free(s->reason_str);
        }
        free(payload);
    }
    uint64_t dropped = atomic_fetch_add_explicit(&g_mb_dropped, 1,
                                                 memory_order_relaxed);
    if ((dropped & 0xFFFF) == 0) {
        fprintf(stderr,
            "swarmrt: mailbox overflow pid=%llu len=%lld cap=%lld from=%llu"
            " — message dropped (%llu dropped total; SW_MAILBOX_MAX tunes,"
            " 0 disables)\n",
            (unsigned long long)to->pid,
            (long long)atomic_load_explicit(&to->mb_len, memory_order_relaxed),
            (long long)g_mailbox_max,
            (unsigned long long)(tls_current ? tls_current->pid : 0),
            (unsigned long long)(dropped + 1));
    }
    mailbox_wake(to);
}

void sw_send(sw_process_t *to, void *msg) {
    if (!to || !msg) return;

    if (!mailbox_admit(to)) {
        mailbox_overflow_drop(to, SW_TAG_NONE, msg, NULL);
        return;
    }

    sw_msg_t *m = msg_alloc();
    m->tag = SW_TAG_NONE;
    m->payload = msg;
    m->from_pid = tls_current ? tls_current->pid : 0;
    m->next = NULL;
    m->prev = NULL;

    /* Lock-free MPSC push */
    mailbox_push(&to->mailbox, m);
    trev(TREV_PUSH, to->pid, m->from_pid);

    /* Wake if waiting (lock-free) */
    mailbox_wake(to);

    if (tls_current) tls_current->messages_sent++;
}

void *sw_receive(uint64_t timeout_ms) {
    sw_process_t *proc = tls_current;
    if (!proc) return NULL;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        /* Drain signal queue into private queue */
        mailbox_drain(&proc->mailbox);

        /* Pop first message from private queue */
        sw_msg_t *m = mailbox_pop_first(proc);
        if (m) {
            void *payload = msg_take_payload(m, 0);
            msg_free(m);
            proc->messages_recv++;
            return payload;
        }

        if (timeout_ms == 0) return NULL;

        /* No message — prepare to sleep.
         * Critical ordering: set waiting BEFORE final drain check.
         * This prevents lost wake-ups (see Vyukov MPSC pattern). */
        atomic_store_explicit(&proc->state, SW_PROC_WAITING, memory_order_relaxed);
        atomic_store_explicit(&proc->mailbox.waiting, 1, memory_order_seq_cst);

        /* Final drain — catch messages sent between first drain and waiting flag.
         * (seq_cst store: see the Dekker note in sw_receive_any — on x86 the
         * drain's locked xchg fenced this anyway; on arm64 it did not.) */
        mailbox_drain(&proc->mailbox);
        m = mailbox_pop_first(proc);
        if (m) {
            /* Got a message — race-safe cancel via atomic_exchange.
             * Only self-resume if WE clear waiting (exchange returns 1).
             * If sender already cleared it (returns 0), process is in the
             * runq — push message back and context-swap to avoid double-run. */
            int was_waiting = atomic_exchange_explicit(&proc->mailbox.waiting, 0, memory_order_acq_rel);
            if (was_waiting) {
                /* We won — not in runq, safe to self-resume */
                atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
                void *payload = msg_take_payload(m, 0);
                msg_free(m);
                proc->messages_recv++;
                return payload;
            }
            /* Sender already enqueued us. Push message back to front of
             * private queue. Keep state=WAITING so scheduler's post-swap
             * handler won't re-enqueue (sender already did). Context-swap
             * out — scheduler will pick us from runq and we'll find it. */
            m->prev = NULL;
            m->next = proc->mailbox.priv_head;
            if (proc->mailbox.priv_head) proc->mailbox.priv_head->prev = m;
            else proc->mailbox.priv_tail = m;
            proc->mailbox.priv_head = m;
            proc->mailbox.count++;
            atomic_fetch_add_explicit(&proc->mb_len, 1, memory_order_relaxed);
            sw_context_swap(proc, &proc->scheduler->sched_proc);
            atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
            if (proc->kill_flag) return NULL;
            continue;  /* Loop back — will drain and find the message */
        }

        /* Truly empty — set up timeout and context swap */
        uint64_t timer_ref = 0;
        if (timeout_ms != (uint64_t)-1) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                              (now.tv_nsec - start.tv_nsec) / 1000000;
            if (elapsed >= timeout_ms) {
                int was_waiting = atomic_exchange_explicit(&proc->mailbox.waiting, 0, memory_order_acq_rel);
                if (was_waiting) {
                    atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
                    return NULL;
                }
                /* Sender already woke us and enqueued to runq — context-swap
                 * to let scheduler properly dequeue before continuing. */
                sw_context_swap(proc, &proc->scheduler->sched_proc);
                atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
                return NULL;
            }
            uint64_t remaining = timeout_ms - elapsed;
            timer_ref = sw_send_after(remaining, proc, SW_TAG_NONE, NULL);
        }

        /* Context swap back to scheduler */
        sw_context_swap(proc, &proc->scheduler->sched_proc);

        /* Resumed — sender or timer woke us (already cleared waiting) */
        atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);

        if (timer_ref) sw_cancel_timer(timer_ref);
        if (proc->kill_flag) return NULL;
    }
}

void *sw_receive_nowait(void) {
    return sw_receive(0);
}

/* ============================================================================
 * SELECTIVE RECEIVE HELPERS
 *
 * These enable OTP-style selective receive: scan the mailbox for a matching
 * message without consuming non-matching ones. The codegen emits:
 *   1. drain signals
 *   2. walk private queue (peek + next)
 *   3. on match: remove_msg, execute body
 *   4. no match: wait_new, goto 1
 * ============================================================================ */

void sw_mailbox_drain_signals(void) {
    sw_process_t *proc = tls_current;
    if (proc) mailbox_drain(&proc->mailbox);
}

sw_msg_t *sw_mailbox_peek(void) {
    sw_process_t *proc = tls_current;
    if (!proc) return NULL;
    mailbox_drain(&proc->mailbox);
    return proc->mailbox.priv_head;
}

void sw_mailbox_remove_msg(sw_msg_t *m) {
    sw_process_t *proc = tls_current;
    if (!proc || !m) return;
    sw_mailbox_t *mb = &proc->mailbox;
    if (m->prev) m->prev->next = m->next;
    else mb->priv_head = m->next;
    if (m->next) m->next->prev = m->prev;
    else mb->priv_tail = m->prev;
    mb->count--;
    atomic_fetch_sub_explicit(&proc->mb_len, 1, memory_order_relaxed);
    proc->messages_recv++;
}

int sw_mailbox_wait_new(uint64_t timeout_ms) {
    sw_process_t *proc = tls_current;
    if (!proc) return 0;
    if (timeout_ms == 0) return 0;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Set waiting flag BEFORE checking signal stack (prevents lost wake-ups) */
    atomic_store_explicit(&proc->state, SW_PROC_WAITING, memory_order_relaxed);
    atomic_store_explicit(&proc->mailbox.waiting, 1, memory_order_seq_cst);

    /* Check if new signals arrived AFTER we set waiting flag.
     *
     * BOTH the store above and this load are seq_cst ON PURPOSE — this
     * is Dekker's pattern (store one var, load a DIFFERENT var, with a
     * peer doing the mirror image in sw_send/mailbox_wake). With the
     * original release-store + acquire-load the store sat in the store
     * buffer past the load (x86 StoreLoad reordering): the receiver saw
     * a pre-push sig_head and parked, while the sender's wake-xchg read
     * waiting==0 and skipped the enqueue — BOTH sides lost, a permanent
     * deadlock with the message sitting in the mailbox. That is the
     * spin-gated P1: parked, waiting=1, sig_head!=NULL (autopsy-proven,
     * ~15%% of depth-1 cross-scheduler ping-pong runs with spin on).
     * TSan is silent — no data race, pure ordering. The three sibling
     * receive paths survive on x86 only because mailbox_drain's locked
     * xchg is a full fence — NOT a guarantee on arm64, so all four
     * waiting-stores and mailbox_wake's xchg are seq_cst now. */
    sw_msg_t *sig = atomic_load_explicit(&proc->mailbox.sig_head, memory_order_seq_cst);
    if (sig) {
        /* New messages on signal stack — drain and let caller re-scan */
        int was = atomic_exchange_explicit(&proc->mailbox.waiting, 0, memory_order_acq_rel);
        if (was) {
            atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
            mailbox_drain(&proc->mailbox);
            return 1;
        }
        /* Sender already enqueued us — context swap for clean dequeue */
        sw_context_swap(proc, &proc->scheduler->sched_proc);
        atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
        mailbox_drain(&proc->mailbox);
        return 1;
    }

    /* No new signals — set up timeout and sleep */
    uint64_t timer_ref = 0;
    if (timeout_ms != (uint64_t)-1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                           (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= timeout_ms) {
            int was = atomic_exchange_explicit(&proc->mailbox.waiting, 0, memory_order_acq_rel);
            if (was) { atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed); return 0; }
            sw_context_swap(proc, &proc->scheduler->sched_proc);
            atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
            return 0;
        }
        timer_ref = sw_send_after(timeout_ms - elapsed, proc, SW_TAG_NONE, NULL);
    }

    /* Context swap — will be woken by sender or timer */
    sw_context_swap(proc, &proc->scheduler->sched_proc);
    atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);

    if (timer_ref) sw_cancel_timer(timer_ref);
    if (proc->kill_flag) return 0;

    /* Drain newly arrived signals */
    mailbox_drain(&proc->mailbox);
    return 1;
}

/* ============================================================================
 * PROCESS EXIT SIGNALS (Crash Propagation)
 *
 * When a process dies:
 * 1. Linked processes get exit signals (kill or message if trapping)
 * 2. Monitors get DOWN messages
 * 3. Registry entry is removed
 * ============================================================================ */

static void deliver_signal(sw_process_t *target, uint64_t tag,
                           uint64_t from_pid, uint64_t ref, int reason,
                           const char *reason_str) {
    /* EXIT/DOWN signals are EXEMPT from the SW_MAILBOX_MAX cap — dropping
     * them would break supervision (a flooded supervisor missing a child's
     * EXIT). Still increment mb_len so the accounting stays exact. */
    atomic_fetch_add_explicit(&target->mb_len, 1, memory_order_relaxed);

    sw_signal_t *sig = (sw_signal_t *)malloc(sizeof(sw_signal_t));
    sig->pid = from_pid;
    sig->ref = ref;
    sig->reason = reason;
    sig->reason_str = reason_str ? strdup(reason_str) : NULL;

    sw_msg_t *m = msg_alloc();
    m->tag = tag;
    m->payload = sig;
    m->from_pid = from_pid;
    m->next = NULL;
    m->prev = NULL;

    /* Lock-free MPSC push + wake */
    mailbox_push(&target->mailbox, m);
    mailbox_wake(target);
}

static void process_exit(sw_process_t *proc, int reason) {
    /* Observability: count every abnormal exit (panic / uncaught error /
     * kill). Cold path, relaxed atomic — see sw_proc_crashes(). */
    if (reason != 0)
        atomic_fetch_add_explicit(&g_proc_crashes, 1, memory_order_relaxed);

    pthread_mutex_lock(&g_swarm->link_lock);
    /* Set the terminal reason UNDER link_lock so sw_monitor's
     * already-dead fast path (which now reads it under the same lock)
     * sees a consistent value instead of racing this write. */
    proc->exit_reason = reason;

    /* 1. Propagate to linked processes */
    sw_link_t *link = proc->links;
    while (link) {
        sw_link_t *next = link->next;
        sw_process_t *peer = link->peer;

        /* Remove proc from peer's link list */
        sw_link_t **pp = &peer->links;
        while (*pp) {
            if ((*pp)->peer == proc) {
                sw_link_t *rm = *pp;
                *pp = rm->next;
                free(rm);
                break;
            }
            pp = &(*pp)->next;
        }

        if (reason != 0) {
            /* Abnormal exit — kill peer or send message */
            if (peer->flags & SW_FLAG_TRAP_EXIT) {
                deliver_signal(peer, SW_TAG_EXIT, proc->pid, 0, reason, proc->panic_msg);
            } else {
                /* Kill the linked process */
                peer->kill_flag = 1;
                peer->exit_reason = reason;
                /* If waiting, wake it so scheduler can clean it up */
                mailbox_wake(peer);
            }
        } else {
            /* Normal exit — only notify if trapping exits */
            if (peer->flags & SW_FLAG_TRAP_EXIT) {
                deliver_signal(peer, SW_TAG_EXIT, proc->pid, 0, 0, NULL);
            }
        }

        free(link);
        link = next;
    }
    proc->links = NULL;

    /* 2. Notify monitors watching this process */
    sw_monitor_t *mon = proc->monitors_me;
    while (mon) {
        sw_monitor_t *next = mon->next_in_watched;

        /* Send DOWN message to watcher */
        deliver_signal(mon->watcher, SW_TAG_DOWN, proc->pid, mon->ref, reason, proc->panic_msg);

        /* Remove from watcher's my_monitors list */
        sw_monitor_t **mp = &mon->watcher->my_monitors;
        while (*mp) {
            if (*mp == mon) {
                *mp = mon->next_in_watcher;
                break;
            }
            mp = &(*mp)->next_in_watcher;
        }

        free(mon);
        mon = next;
    }
    proc->monitors_me = NULL;

    /* 3. Clean up monitors this process created (no longer watching) */
    sw_monitor_t *mymon = proc->my_monitors;
    while (mymon) {
        sw_monitor_t *next = mymon->next_in_watcher;

        /* Remove from watched's monitors_me list */
        sw_monitor_t **mp = &mymon->watched->monitors_me;
        while (*mp) {
            if (*mp == mymon) {
                *mp = mymon->next_in_watched;
                break;
            }
            mp = &(*mp)->next_in_watched;
        }

        free(mymon);
        mymon = next;
    }
    proc->my_monitors = NULL;

    pthread_mutex_unlock(&g_swarm->link_lock);

    /* 3b. Observability: structured crash record (opt-in, SW_LOG_JSON=1).
     * Emitted AFTER the lock is released (fprintf under link_lock would
     * serialize teardown on stderr) and BEFORE step 6 unregisters, so the
     * record still carries the registered name. Reuses THIS exit path —
     * no second exit hook. */
    if (reason != 0 && log_json_enabled())
        emit_crash_json(proc, reason);

    /* 4. Clean up owned ETS tables */
    if (proc->ets_tables) {
        sw_ets_cleanup_owner(proc);
    }

    /* 5. Clean up process group memberships */
    sw_pg_cleanup_proc(proc);

    /* 5b. Clean up module tracking */
    sw_module_cleanup_proc(proc);

    /* 6. Unregister from registry */
    if (proc->reg_entry) {
        registry_remove_proc(proc);
    }
}

/* ============================================================================
 * LINKS
 * ============================================================================ */

int sw_link(sw_process_t *other) {
    sw_process_t *self = tls_current;
    if (!self || !other || self == other) return -1;
    if (other->state == SW_PROC_EXITING || other->state == SW_PROC_FREE) {
        /* Linking to dead process — deliver exit signal immediately */
        if (self->flags & SW_FLAG_TRAP_EXIT) {
            deliver_signal(self, SW_TAG_EXIT, other->pid, 0, other->exit_reason, other->panic_msg);
        } else if (other->exit_reason != 0) {
            self->kill_flag = 1;
            self->exit_reason = other->exit_reason;
        }
        return 0;
    }

    pthread_mutex_lock(&g_swarm->link_lock);

    /* Check if already linked */
    sw_link_t *l = self->links;
    while (l) {
        if (l->peer == other) {
            pthread_mutex_unlock(&g_swarm->link_lock);
            return 0; /* Already linked */
        }
        l = l->next;
    }

    /* Add other to self's link list */
    sw_link_t *la = (sw_link_t *)malloc(sizeof(sw_link_t));
    la->peer = other;
    la->next = self->links;
    self->links = la;

    /* Add self to other's link list */
    sw_link_t *lb = (sw_link_t *)malloc(sizeof(sw_link_t));
    lb->peer = self;
    lb->next = other->links;
    other->links = lb;

    pthread_mutex_unlock(&g_swarm->link_lock);
    return 0;
}

int sw_unlink(sw_process_t *other) {
    sw_process_t *self = tls_current;
    if (!self || !other) return -1;

    pthread_mutex_lock(&g_swarm->link_lock);

    /* Remove other from self's link list */
    sw_link_t **pp = &self->links;
    while (*pp) {
        if ((*pp)->peer == other) {
            sw_link_t *rm = *pp;
            *pp = rm->next;
            free(rm);
            break;
        }
        pp = &(*pp)->next;
    }

    /* Remove self from other's link list */
    pp = &other->links;
    while (*pp) {
        if ((*pp)->peer == self) {
            sw_link_t *rm = *pp;
            *pp = rm->next;
            free(rm);
            break;
        }
        pp = &(*pp)->next;
    }

    pthread_mutex_unlock(&g_swarm->link_lock);
    return 0;
}

sw_process_t *sw_spawn_link(void (*func)(void*), void *arg) {
    /* Pin the child to a scheduler that is NOT the parent's. In
     * cooperative mode the parent often blocks waiting for the child
     * (usleep, sw_receive without a producer, etc.) — if both end up
     * on the same scheduler the child can't run, the parent never
     * unblocks, and the test sees missing members / lost messages.
     *
     * The pin is published via tls_spawn_override (a separate TLS slot
     * from tls_scheduler — overwriting tls_scheduler doesn't influence
     * sw_spawn's scheduler pick, which was the original bug behind
     * phase5 pg_* failures). sw_spawn_opts reads tls_spawn_override
     * and uses it as the scheduler id; we clear it after the spawn. */
    sw_scheduler_t *parent_sched = tls_scheduler;
    if (parent_sched && g_swarm->num_schedulers > 1) {
        uint32_t target;
        do {
            target = __sync_fetch_and_add(&g_swarm->next_sched, 1)
                     % g_swarm->num_schedulers;
        } while (target == parent_sched->id);
        tls_spawn_override = g_swarm->schedulers[target];
    }
    sw_process_t *child = sw_spawn(func, arg);
    tls_spawn_override = NULL;
    if (!child) return NULL;

    sw_process_t *self = tls_current;
    if (self) {
        /* Set up link before child can run */
        pthread_mutex_lock(&g_swarm->link_lock);

        sw_link_t *la = (sw_link_t *)malloc(sizeof(sw_link_t));
        la->peer = child;
        la->next = self->links;
        self->links = la;

        sw_link_t *lb = (sw_link_t *)malloc(sizeof(sw_link_t));
        lb->peer = self;
        lb->next = child->links;
        child->links = lb;

        child->parent = self;

        pthread_mutex_unlock(&g_swarm->link_lock);
    }
    return child;
}

/* ============================================================================
 * MONITORS
 * ============================================================================ */

uint64_t sw_monitor(sw_process_t *target) {
    sw_process_t *self = tls_current;
    if (!self || !target) return 0;

    uint64_t ref = atomic_fetch_add(&g_swarm->next_monitor_ref, 1);

    /* The dead-check, the terminal-reason read, AND the registration all
     * happen under link_lock — the same lock process_exit holds while it
     * finalizes exit_reason and walks monitors_me. This (a) closes the
     * data race on exit_reason/panic_msg (TSan-flagged: monitoring a
     * process at its exact death instant), and (b) makes delivery
     * exactly-once: if we register before process_exit locks, it finds us
     * and delivers DOWN; if the target is already EXITING/FREE when we
     * lock, we deliver DOWN ourselves and do NOT register. */
    pthread_mutex_lock(&g_swarm->link_lock);

    sw_proc_state_t st = atomic_load_explicit(&target->state, memory_order_relaxed);
    if (st == SW_PROC_EXITING || st == SW_PROC_FREE) {
        int reason = atomic_load_explicit(&target->exit_reason, memory_order_relaxed);
        /* COPY panic_msg under the lock: process_destroy frees it under the
         * same lock, so capturing the bare pointer and using it after the
         * unlock (deliver_signal strdups it) would race that free. The
         * copy is ours; free it after delivery. */
        char *msg = target->panic_msg ? strdup(target->panic_msg) : NULL;
        pthread_mutex_unlock(&g_swarm->link_lock);
        deliver_signal(self, SW_TAG_DOWN, target->pid, ref, reason, msg);
        free(msg);
        return ref;
    }

    sw_monitor_t *mon = (sw_monitor_t *)malloc(sizeof(sw_monitor_t));
    mon->ref = ref;
    mon->watcher = self;
    mon->watched = target;

    /* Add to watcher's my_monitors */
    mon->next_in_watcher = self->my_monitors;
    self->my_monitors = mon;

    /* Add to watched's monitors_me */
    mon->next_in_watched = target->monitors_me;
    target->monitors_me = mon;

    pthread_mutex_unlock(&g_swarm->link_lock);

    return ref;
}

int sw_demonitor(uint64_t ref) {
    sw_process_t *self = tls_current;
    if (!self || ref == 0) return -1;

    pthread_mutex_lock(&g_swarm->link_lock);

    /* Find in self's my_monitors */
    sw_monitor_t **mp = &self->my_monitors;
    sw_monitor_t *mon = NULL;
    while (*mp) {
        if ((*mp)->ref == ref) {
            mon = *mp;
            *mp = mon->next_in_watcher;
            break;
        }
        mp = &(*mp)->next_in_watcher;
    }

    if (mon) {
        /* Remove from watched's monitors_me */
        sw_monitor_t **wp = &mon->watched->monitors_me;
        while (*wp) {
            if (*wp == mon) {
                *wp = mon->next_in_watched;
                break;
            }
            wp = &(*wp)->next_in_watched;
        }
        free(mon);
    }

    pthread_mutex_unlock(&g_swarm->link_lock);
    return mon ? 0 : -1;
}

/* ============================================================================
 * PROCESS FLAGS
 * ============================================================================ */

void sw_process_flag(uint32_t flag, int value) {
    sw_process_t *self = tls_current;
    if (!self) return;
    if (value)
        self->flags |= flag;
    else
        self->flags &= ~flag;
}

/* ============================================================================
 * PROCESS REGISTRY
 * ============================================================================ */

static uint32_t registry_hash(const char *name) {
    uint32_t h = 2166136261u; /* FNV-1a */
    for (const char *p = name; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return h;
}

int sw_register(const char *name, sw_process_t *proc) {
    if (!g_swarm || !name || !proc) return -1;
    if (proc->reg_entry) return -1; /* Already registered */

    sw_registry_t *reg = &g_swarm->registry;
    uint32_t idx = registry_hash(name) % reg->num_buckets;

    pthread_rwlock_wrlock(&reg->lock);

    /* Check for duplicate name */
    sw_reg_entry_t *e = reg->buckets[idx];
    while (e) {
        if (strncmp(e->name, name, SW_REG_NAME_MAX - 1) == 0) {
            pthread_rwlock_unlock(&reg->lock);
            return -1; /* Name already taken */
        }
        e = e->next;
    }

    /* Create entry */
    sw_reg_entry_t *entry = (sw_reg_entry_t *)malloc(sizeof(sw_reg_entry_t));
    strncpy(entry->name, name, SW_REG_NAME_MAX - 1);
    entry->name[SW_REG_NAME_MAX - 1] = '\0';
    entry->proc = proc;
    entry->next = reg->buckets[idx];
    reg->buckets[idx] = entry;
    proc->reg_entry = entry;

    pthread_rwlock_unlock(&reg->lock);
    return 0;
}

int sw_unregister(const char *name) {
    if (!g_swarm || !name) return -1;

    sw_registry_t *reg = &g_swarm->registry;
    uint32_t idx = registry_hash(name) % reg->num_buckets;

    pthread_rwlock_wrlock(&reg->lock);

    sw_reg_entry_t **pp = &reg->buckets[idx];
    while (*pp) {
        if (strncmp((*pp)->name, name, SW_REG_NAME_MAX - 1) == 0) {
            sw_reg_entry_t *rm = *pp;
            *pp = rm->next;
            rm->proc->reg_entry = NULL;
            free(rm);
            pthread_rwlock_unlock(&reg->lock);
            return 0;
        }
        pp = &(*pp)->next;
    }

    pthread_rwlock_unlock(&reg->lock);
    return -1; /* Not found */
}

sw_process_t *sw_whereis(const char *name) {
    if (!g_swarm || !name) return NULL;

    sw_registry_t *reg = &g_swarm->registry;
    uint32_t idx = registry_hash(name) % reg->num_buckets;

    pthread_rwlock_rdlock(&reg->lock);

    sw_reg_entry_t *e = reg->buckets[idx];
    while (e) {
        if (strncmp(e->name, name, SW_REG_NAME_MAX - 1) == 0) {
            sw_process_t *proc = e->proc;
            pthread_rwlock_unlock(&reg->lock);
            return proc;
        }
        e = e->next;
    }

    pthread_rwlock_unlock(&reg->lock);
    return NULL;
}

static void registry_remove_proc(sw_process_t *proc) {
    sw_reg_entry_t *entry = atomic_load_explicit(&proc->reg_entry, memory_order_relaxed);
    if (!entry) return;

    sw_registry_t *reg = &g_swarm->registry;

    /* Hash the name UNDER the wrlock. It was computed before the lock,
     * reading entry->name — which races a concurrent registration/removal
     * reusing that entry's storage (TSan-flagged in the supervisor
     * crash-restart path). The entry is only freed under this lock, so the
     * name is stable once we hold it. */
    pthread_rwlock_wrlock(&reg->lock);

    uint32_t idx = registry_hash(entry->name) % reg->num_buckets;
    sw_reg_entry_t **pp = &reg->buckets[idx];
    while (*pp) {
        if (*pp == entry) {
            sw_reg_entry_t *rm = *pp;
            *pp = rm->next;
            free(rm);
            break;
        }
        pp = &(*pp)->next;
    }
    atomic_store_explicit(&proc->reg_entry, NULL, memory_order_relaxed);

    pthread_rwlock_unlock(&reg->lock);
}

int sw_send_named(const char *name, uint64_t tag, void *msg) {
    sw_process_t *proc = sw_whereis(name);
    if (!proc) return -1;
    if (tag == SW_TAG_NONE)
        sw_send(proc, msg);
    else
        sw_send_tagged(proc, tag, msg);
    return 0;
}

/* ============================================================================
 * TIMERS
 * ============================================================================ */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t sw_send_after(uint64_t delay_ms, sw_process_t *dest, uint64_t tag, void *msg) {
    if (!g_swarm || !dest) return 0;

    uint64_t ref = atomic_fetch_add(&g_swarm->next_timer_ref, 1);

    sw_timer_t *t = timer_alloc();
    t->ref = ref;
    t->fire_at_ns = now_ns() + delay_ms * 1000000ULL;
    t->dest = dest;
    t->tag = tag;
    t->msg = msg;
    t->next = NULL;

    /* Insert sorted by fire_at_ns */
    sw_timer_list_t *tl = &g_swarm->timers;
    pthread_mutex_lock(&tl->lock);

    sw_timer_t **pp = &tl->head;
    while (*pp && (*pp)->fire_at_ns <= t->fire_at_ns) {
        pp = &(*pp)->next;
    }
    t->next = *pp;
    *pp = t;

    pthread_mutex_unlock(&tl->lock);
    return ref;
}

int sw_cancel_timer(uint64_t ref) {
    if (!g_swarm || ref == 0) return -1;

    sw_timer_list_t *tl = &g_swarm->timers;
    pthread_mutex_lock(&tl->lock);

    sw_timer_t **pp = &tl->head;
    while (*pp) {
        if ((*pp)->ref == ref) {
            sw_timer_t *rm = *pp;
            *pp = rm->next;
            free(rm->msg);
            timer_free(rm);
            pthread_mutex_unlock(&tl->lock);
            return 0;
        }
        pp = &(*pp)->next;
    }

    pthread_mutex_unlock(&tl->lock);
    return -1; /* Not found */
}

/* Defined below with sw_send_tagged; forward-declared so fire_timers can take
 * the UNCAPPED path (capped=0) — timer fires must never be dropped, the
 * receive-after wake-ups depend on them. */
static void send_tagged_internal(sw_process_t *to, uint64_t tag, void *msg,
                                 int capped);

static void fire_timers(void) {
    if (!g_swarm) return;

    sw_timer_list_t *tl = &g_swarm->timers;

    /* Peek under the lock. The old unlocked `if (!tl->head) return` raced
     * the locked insert in sw_send_after (TSan-flagged; benign on x86/arm64
     * where aligned pointer loads are atomic, but UB on paper). The lock is
     * uncontended in the common no-timer case — taken once per scheduler
     * idle pass, contended only while a timer is actually being
     * inserted/cancelled (rare) — so the correctness is free in practice. */
    uint64_t now = now_ns();

    pthread_mutex_lock(&tl->lock);
    if (!tl->head) { pthread_mutex_unlock(&tl->lock); return; }
    while (tl->head && tl->head->fire_at_ns <= now) {
        sw_timer_t *t = tl->head;
        tl->head = t->next;
        pthread_mutex_unlock(&tl->lock);

        if (t->msg == NULL && t->tag == SW_TAG_NONE) {
            /* Wake-up timer (from receive timeout) — just wake process */
            mailbox_wake(t->dest);
        } else {
            /* Regular timer — deliver the message. UNCAPPED (capped=0):
             * a timer fire dropped by a full mailbox would silently break
             * receive-after/send_after semantics on a flooded process. */
            send_tagged_internal(t->dest, t->tag, t->msg, 0);
        }
        timer_free(t);

        pthread_mutex_lock(&tl->lock);
    }
    pthread_mutex_unlock(&tl->lock);
}

/* ============================================================================
 * GRACEFUL SHUTDOWN — drain-with-deadline (Phase 4)
 * ============================================================================ */

/* Is the node quiescent — no outstanding work to drain? True when every live,
 * non-scheduler process is parked (WAITING/SUSPENDED/EXITING) with an EMPTY
 * mailbox and the global overflow run-queue is empty. Mirrors the watchdog
 * scan: best-effort, lock-free reads of per-process state/mailbox (a message in
 * flight at the scan instant just costs one more poll). A RUNNABLE/RUNNING
 * process, a non-empty mailbox (sig_head/priv_head/mb_len), or queued overflow
 * work all mean "not yet drained". Pending TIMERS are deliberately NOT counted
 * as outstanding work — a heartbeat/interval timer would otherwise keep the
 * node "busy" forever; they are cancelled explicitly (cancel_pending_timers). */
static int runtime_is_quiescent(void) {
    sw_swarm_t *sw = g_swarm;
    if (!sw) return 1;
    sw_process_t *slab = (sw_process_t *)sw->arena.proc_slab;
    if (!slab) return 1;
    uint32_t cap = sw->arena.proc_capacity;

    uint64_t sched_pids[SWARM_MAX_SCHEDULERS];
    uint32_t nsched = sw->num_schedulers;
    if (nsched > SWARM_MAX_SCHEDULERS) nsched = SWARM_MAX_SCHEDULERS;
    for (uint32_t s = 0; s < nsched; s++) {
        sw_scheduler_t *sc = sw->schedulers[s];
        sched_pids[s] = sc ? sc->sched_proc.pid : (uint64_t)-1;
    }

    for (uint32_t i = 0; i < cap; i++) {
        sw_process_t *p = &slab[i];
        sw_proc_state_t st = atomic_load_explicit(&p->state, memory_order_acquire);
        if (st == SW_PROC_FREE) continue;

        uint64_t pid = p->pid;
        int is_sched = 0;
        for (uint32_t s = 0; s < nsched; s++)
            if (pid == sched_pids[s]) { is_sched = 1; break; }
        if (is_sched) continue;

        /* Still on/heading-to a run queue = work in progress. */
        if (st == SW_PROC_RUNNABLE || st == SW_PROC_RUNNING) return 0;

        /* Pending mailbox = a parked receiver about to be woken with work. */
        sw_msg_t *sig = (sw_msg_t *)atomic_load_explicit(
            &p->mailbox.sig_head, memory_order_acquire);
        if (sig || p->mailbox.priv_head) return 0;
        if (atomic_load_explicit(&p->mb_len, memory_order_relaxed) > 0) return 0;
    }

    pthread_mutex_lock(&sw->overflow_rq.lock);
    uint32_t oc = sw->overflow_rq.count;
    pthread_mutex_unlock(&sw->overflow_rq.lock);
    if (oc > 0) return 0;

    return 1;
}

/* Cancel every pending timer (the "cancel timers" step): unlink the whole list
 * under its lock, then free each node + its RAW payload (matching sw_shutdown's
 * own timer teardown — plain free, not the tls freelist, so nothing accumulates
 * on the shutting-down thread). Schedulers may still be running: fire_timers
 * re-locks and finds an empty list. A timer a still-draining fiber arms after
 * this is reclaimed by sw_shutdown's own timer sweep, so nothing leaks. */
static void cancel_pending_timers(void) {
    if (!g_swarm) return;
    sw_timer_list_t *tl = &g_swarm->timers;
    pthread_mutex_lock(&tl->lock);
    sw_timer_t *t = tl->head;
    tl->head = NULL;
    pthread_mutex_unlock(&tl->lock);
    while (t) {
        sw_timer_t *next = t->next;
        free(t->msg);
        free(t);
        t = next;
    }
}

int sw_shutdown_graceful(int swarm_id, int deadline_ms) {
    if (!g_swarm) return 0;
    if (deadline_ms < 0) deadline_ms = sw_shutdown_grace_ms();

    int quiet = !getenv("SW_QUIET") && !getenv("SW_RUNTIME_QUIET");

    /* 1. Stop accepting new work: flip the draining flag. Observable via
     *    sw_is_draining()/swarm_stats() → a readiness probe (/readyz) can fail
     *    so a load balancer stops routing new external requests. */
    atomic_store_explicit(&g_sw_draining, 1, memory_order_release);
    if (quiet) {
        fprintf(stderr, "[SwarmRT] graceful shutdown: draining (deadline %dms)\n",
                deadline_ms);
        fflush(stderr);
    }

    /* 2. Drain outstanding messages: let runnable fibers finish and mailboxes
     *    empty, polling for quiescence up to the deadline. */
    uint64_t start = now_ns();
    uint64_t deadline_ns = start + (uint64_t)deadline_ms * 1000000ULL;
    int drained = 0;
    while (1) {
        if (runtime_is_quiescent()) { drained = 1; break; }
        if (now_ns() >= deadline_ns) break;
        usleep(2000);   /* 2 ms poll — negligible vs the drain window */
    }

    /* 3. Cancel timers: discard any pending fire so nothing re-injects work. */
    cancel_pending_timers();

    if (quiet) {
        uint64_t took_ms = (now_ns() - start) / 1000000ULL;
        if (drained)
            fprintf(stderr, "[SwarmRT] graceful shutdown: drained in %llums\n",
                    (unsigned long long)took_ms);
        else
            fprintf(stderr, "[SwarmRT] graceful shutdown: deadline reached after"
                    " %llums with work outstanding — forcing teardown\n",
                    (unsigned long long)took_ms);
        fflush(stderr);
    }

    /* 4. Flush storage / terminate. The hard teardown joins the scheduler
     *    threads — BOUNDED even for a hung (never-quiescing) fiber, because
     *    reduction preemption returns each fiber to its scheduler, which then
     *    honours should_exit. process_destroy fires on_destroy per process.
     *    SQLite durability is per-statement autocommit; the drain above let
     *    in-flight writers finish (see docs/DEPLOYMENT.md). */
    sw_shutdown(swarm_id);
    return drained ? 0 : 1;
}

/* ============================================================================
 * TAGGED MESSAGES & SELECTIVE RECEIVE
 * ============================================================================ */

/* Shared body for sw_send_tagged (capped=1, user sends) and timer fires
 * (capped=0 — exempt from SW_MAILBOX_MAX, but still counted in mb_len so the
 * accounting stays exact; see fire_timers). */
static void send_tagged_internal(sw_process_t *to, uint64_t tag, void *msg,
                                 int capped) {
    if (!to) return;

    if (capped) {
        if (!mailbox_admit(to)) {
            mailbox_overflow_drop(to, tag, msg, NULL);
            return;
        }
    } else {
        atomic_fetch_add_explicit(&to->mb_len, 1, memory_order_relaxed);
    }

    sw_msg_t *m = msg_alloc();
    m->tag = tag;
    m->payload = msg;
    m->from_pid = tls_current ? tls_current->pid : 0;
    m->next = NULL;
    m->prev = NULL;
    /* region=NULL, pkind=SW_PK_RAW already set by msg_alloc — RAW payload path. */

    /* Lock-free MPSC push + wake */
    mailbox_push(&to->mailbox, m);
    mailbox_wake(to);

    if (tls_current) tls_current->messages_sent++;
}

void sw_send_tagged(sw_process_t *to, uint64_t tag, void *msg) {
    send_tagged_internal(to, tag, msg, 1);
}

/* Ownership v2: enqueue an sw_val_t VALUE payload OWNED by `region`. The region
 * holds the deep-copied graph while queued; the receiver adopts it on match
 * (codegen), and process_destroy bulk-frees it if undelivered. Distinct from the
 * RAW sw_send_tagged so signals/gen_server/port payloads are never region-freed. */
void sw_send_tagged_msg(sw_process_t *to, uint64_t tag, void *payload,
                        struct sw_value_arena *region) {
    if (!to) return;

    /* Message-size cap (SW_MSG_MAX_BYTES): the region's total_bytes IS the
     * deep-copied payload's size. Drop mirrors mailbox_overflow_drop: bulk-
     * free the region (the whole graph lives in it — never free(payload)),
     * count + warn rate-limited, and CRITICALLY wake the receiver anyway —
     * a parked receiver whose inbound is all oversized would otherwise never
     * re-check its queue (livelock); a spurious wake is harmless. EXIT/DOWN
     * are exempt by construction (they ride deliver_signal, not this path). */
    if (g_msg_max_bytes && region && region->total_bytes > g_msg_max_bytes) {
        size_t sz = region->total_bytes;
        sw_varena_free_all(region);
        uint64_t dropped = atomic_fetch_add_explicit(&g_msgsize_dropped, 1,
                                                     memory_order_relaxed);
        if ((dropped & 0x3F) == 0) {
            fprintf(stderr,
                "swarmrt: message of %zu bytes exceeds SW_MSG_MAX_BYTES=%zu"
                " pid=%llu from=%llu — message dropped (%llu dropped total;"
                " 0 disables)\n",
                sz, g_msg_max_bytes,
                (unsigned long long)to->pid,
                (unsigned long long)(tls_current ? tls_current->pid : 0),
                (unsigned long long)(dropped + 1));
        }
        mailbox_wake(to);
        return;
    }

    if (!mailbox_admit(to)) {
        /* VALUE drop: the payload graph lives entirely in `region` —
         * mailbox_overflow_drop bulk-frees the region, never free(payload). */
        mailbox_overflow_drop(to, tag, payload, region);
        return;
    }

    sw_msg_t *m = msg_alloc();
    m->tag = tag;
    m->payload = payload;
    m->from_pid = tls_current ? tls_current->pid : 0;
    m->next = NULL;
    m->prev = NULL;
    m->region = region;
    m->pkind = SW_PK_VALUE;

    mailbox_push(&to->mailbox, m);
    mailbox_wake(to);

    if (tls_current) tls_current->messages_sent++;
}

/*
 * sw_receive_tagged: Selective receive — scan mailbox for first message
 * matching tag, skip non-matching messages (they stay in the mailbox).
 *
 * Selective receive pattern. Essential for request/response:
 *   ref = send_request(server);
 *   reply = sw_receive_tagged(ref, 5000);  // Only get MY reply
 */
void *sw_receive_tagged(uint64_t tag, uint64_t timeout_ms) {
    sw_process_t *proc = tls_current;
    if (!proc) return NULL;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        /* Drain signal queue and scan private queue for matching tag */
        mailbox_drain(&proc->mailbox);
        sw_msg_t *m = mailbox_pop_tagged(proc, tag);
        if (m) {
            void *payload = msg_take_payload(m, 0);
            msg_free(m);
            proc->messages_recv++;
            return payload;
        }

        if (timeout_ms == 0) return NULL;

        /* No match — prepare to sleep */
        atomic_store_explicit(&proc->state, SW_PROC_WAITING, memory_order_relaxed);
        atomic_store_explicit(&proc->mailbox.waiting, 1, memory_order_seq_cst);

        /* Final drain — catch messages sent between first drain and waiting flag.
         * (seq_cst store: see the Dekker note in sw_receive_any — on x86 the
         * drain's locked xchg fenced this anyway; on arm64 it did not.) */
        mailbox_drain(&proc->mailbox);
        m = mailbox_pop_tagged(proc, tag);
        if (m) {
            int was_waiting = atomic_exchange_explicit(&proc->mailbox.waiting, 0, memory_order_acq_rel);
            if (was_waiting) {
                atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
                void *payload = msg_take_payload(m, 0);
                msg_free(m);
                proc->messages_recv++;
                return payload;
            }
            /* Race: sender enqueued us. Push tagged message back and context-swap. */
            m->prev = NULL;
            m->next = proc->mailbox.priv_head;
            if (proc->mailbox.priv_head) proc->mailbox.priv_head->prev = m;
            else proc->mailbox.priv_tail = m;
            proc->mailbox.priv_head = m;
            proc->mailbox.count++;
            atomic_fetch_add_explicit(&proc->mb_len, 1, memory_order_relaxed);
            sw_context_swap(proc, &proc->scheduler->sched_proc);
            atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
            if (proc->kill_flag) return NULL;
            continue;
        }

        /* Truly no match — set up timeout and context swap */
        uint64_t timer_ref = 0;
        if (timeout_ms != (uint64_t)-1) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                                  (now.tv_nsec - start.tv_nsec) / 1000000;
            if (elapsed_ms >= timeout_ms) {
                int was_waiting = atomic_exchange_explicit(&proc->mailbox.waiting, 0, memory_order_acq_rel);
                if (was_waiting) {
                    atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
                    return NULL;
                }
                /* Sender already woke us and enqueued to runq — context-swap
                 * to let scheduler properly dequeue before continuing. */
                sw_context_swap(proc, &proc->scheduler->sched_proc);
                atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
                return NULL;
            }
            uint64_t remaining = timeout_ms - elapsed_ms;
            timer_ref = sw_send_after(remaining, proc, SW_TAG_NONE, NULL);
        }

        sw_context_swap(proc, &proc->scheduler->sched_proc);

        /* Resumed — sender or timer woke us (already cleared waiting) */
        atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
        if (timer_ref) sw_cancel_timer(timer_ref);
        if (proc->kill_flag) return NULL;
    }
}

/*
 * sw_receive_any: Receive any message, returning the tag.
 * Used by GenServer loop to dispatch on message type. `adopt` (a parameter, not
 * a thread-local — it must survive the blocking context switch on THIS fiber's
 * stack, never leak to another fiber) selects region adoption vs materialize.
 */
static void *sw_receive_any_impl(uint64_t timeout_ms, uint64_t *out_tag, int adopt) {
    sw_process_t *proc = tls_current;
    if (!proc) return NULL;
    if (out_tag) *out_tag = SW_TAG_NONE;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        /* Drain signal queue into private queue */
        mailbox_drain(&proc->mailbox);

        sw_msg_t *m = mailbox_pop_first(proc);
        if (m) {
            if (out_tag) *out_tag = m->tag;
            void *payload = msg_take_payload(m, adopt);
            msg_free(m);
            proc->messages_recv++;
            return payload;
        }

        if (timeout_ms == 0) return NULL;

        /* No message — prepare to sleep */
        atomic_store_explicit(&proc->state, SW_PROC_WAITING, memory_order_relaxed);
        atomic_store_explicit(&proc->mailbox.waiting, 1, memory_order_seq_cst);

        /* Final drain — catch messages sent between first drain and waiting flag.
         * (seq_cst store: see the Dekker note in sw_receive_any — on x86 the
         * drain's locked xchg fenced this anyway; on arm64 it did not.) */
        mailbox_drain(&proc->mailbox);
        m = mailbox_pop_first(proc);
        if (m) {
            int was_waiting = atomic_exchange_explicit(&proc->mailbox.waiting, 0, memory_order_acq_rel);
            if (was_waiting) {
                atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
                if (out_tag) *out_tag = m->tag;
                void *payload = msg_take_payload(m, adopt);
                msg_free(m);
                proc->messages_recv++;
                return payload;
            }
            /* Race: sender enqueued us. Push message back and context-swap. */
            m->prev = NULL;
            m->next = proc->mailbox.priv_head;
            if (proc->mailbox.priv_head) proc->mailbox.priv_head->prev = m;
            else proc->mailbox.priv_tail = m;
            proc->mailbox.priv_head = m;
            proc->mailbox.count++;
            atomic_fetch_add_explicit(&proc->mb_len, 1, memory_order_relaxed);
            sw_context_swap(proc, &proc->scheduler->sched_proc);
            atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
            if (proc->kill_flag) return NULL;
            continue;
        }

        /* Truly empty — set up timeout and context swap */
        uint64_t timer_ref = 0;
        if (timeout_ms != (uint64_t)-1) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                              (now.tv_nsec - start.tv_nsec) / 1000000;
            if (elapsed >= timeout_ms) {
                int was_waiting = atomic_exchange_explicit(&proc->mailbox.waiting, 0, memory_order_acq_rel);
                if (was_waiting) {
                    atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
                    return NULL;
                }
                /* Sender already woke us and enqueued to runq — context-swap
                 * to let scheduler properly dequeue before continuing. */
                sw_context_swap(proc, &proc->scheduler->sched_proc);
                atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
                return NULL;
            }
            uint64_t remaining = timeout_ms - elapsed;
            timer_ref = sw_send_after(remaining, proc, SW_TAG_NONE, NULL);
        }

        sw_context_swap(proc, &proc->scheduler->sched_proc);

        /* Resumed — sender or timer woke us (already cleared waiting) */
        atomic_store_explicit(&proc->state, SW_PROC_RUNNING, memory_order_relaxed);
        if (timer_ref) sw_cancel_timer(timer_ref);
        if (proc->kill_flag) return NULL;
    }
}

/* Public sw_receive_any: materialize VALUE payloads to a free-able global copy. */
void *sw_receive_any(uint64_t timeout_ms, uint64_t *out_tag) {
    return sw_receive_any_impl(timeout_ms, out_tag, 0);
}

/* Ownership v2: like sw_receive_any but ADOPTS a VALUE message's region into the
 * current process's arena instead of materializing a free-able global copy. For
 * consumers that incorporate the payload into a returned value (pmap) — the
 * caller must NOT free the returned payload. `adopt` is threaded as a parameter
 * (NOT a thread-local) so it can't leak across the blocking receive's context
 * switch into another fiber. */
void *sw_recv_any_adopt(uint64_t timeout_ms, uint64_t *out_tag) {
    return sw_receive_any_impl(timeout_ms, out_tag, 1);
}

/* ============================================================================
 * STATISTICS
 * ============================================================================ */

void sw_stats(int swarm_id) {
    (void)swarm_id;

    if (!g_swarm) {
        printf("Swarm not initialized\n");
        return;
    }

    sw_arena_t *arena = &g_swarm->arena;
    uint32_t free_slots = 0, free_blocks = 0;
    for (uint32_t p = 0; p < arena->num_partitions; p++) {
        free_slots += arena->partitions[p].pid_top;
        free_blocks += arena->partitions[p].block_top;
    }

    printf("\n=== SwarmRT '%s' Statistics ===\n", g_swarm->name);
    printf("Schedulers: %d\n", g_swarm->num_schedulers);
    printf("Total processes spawned: %llu\n",
           (unsigned long long)atomic_load(&g_swarm->total_spawns));
    printf("Total reductions: %llu\n", (unsigned long long)g_swarm->total_reductions);
    printf("Total messages sent: %llu\n",
           (unsigned long long)atomic_load(&g_swarm->total_sends));
    printf("Next PID: %llu\n",
           (unsigned long long)atomic_load(&arena->next_pid));
    printf("Arena: %zu MB | Slots: %u/%u free | Blocks: %u/%u free\n",
           arena->size / (1024 * 1024),
           free_slots, arena->proc_capacity,
           free_blocks, arena->block_count);

    for (uint32_t i = 0; i < g_swarm->num_schedulers; i++) {
        sw_scheduler_t *sched = g_swarm->schedulers[i];
        printf("  Scheduler %d: run=%llu, iters=%llu, idles=%llu, steals=%llu\n",
               i,
               (unsigned long long)sched->procs_run,
               (unsigned long long)sched->loop_iters,
               (unsigned long long)sched->idle_waits,
               (unsigned long long)sched->steal_attempts);
    }
    printf("================================\n\n");
}

uint64_t sw_process_count(int swarm_id) {
    (void)swarm_id;
    return g_swarm ? atomic_load(&g_swarm->arena.next_pid) : 0;
}
