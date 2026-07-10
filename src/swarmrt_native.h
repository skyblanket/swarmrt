/*
 * SwarmRT - Lightweight Process Runtime
 *
 * Core innovations:
 * - User-space threads (not pthreads)
 * - Small growable stacks (2KB initial, not 64KB)
 * - True preemptive scheduling via reduction counting
 * - Assembly context switching (~100ns target)
 * - Per-process heaps with copying message passing
 *
 * otonomy.ai
 */

#ifndef SWARMRT_NATIVE_H
#define SWARMRT_NATIVE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "swarmrt_arena.h"

/* === Configuration === */
#define SWARM_MAX_PROCESSES    100000    /* 100K processes (reduced from 1M) */
#define SWARM_MAX_SCHEDULERS   64
#define SWARM_STACK_MIN_SIZE   (2 * 1024)   /* 2KB initial stack (not 64KB!) */
#define SWARM_STACK_MAX_SIZE   (1024 * 1024) /* 1MB max */
#define SWARM_HEAP_MIN_SIZE    256       /* 256 words = 2KB initial heap */
#define SWARM_CONTEXT_REDS     2000      /* Reductions per time slice */
#define SWARM_TIME_SLICE_US    1000      /* 1ms max time slice */

/* Default mailbox depth cap (pending messages per process). Override with the
 * SW_MAILBOX_MAX env var (parsed once in sw_init; 0 = unbounded). USER sends
 * over the cap are DROPPED (loud — counter + rate-limited stderr warning);
 * EXIT/DOWN signals and timer fires are exempt so supervision and
 * receive-after semantics stay intact. The cap is approximate under
 * concurrency (overshoot bounded by #concurrent senders). */
#define SW_MAILBOX_MAX_DEFAULT 1000000

/* === Deadlock watchdog ===
 * A background thread that wakes every SW_DEADLOCK_MS milliseconds (default
 * 5000) and checks whether every live non-scheduler process is blocked in
 * sw_receive with an empty mailbox.  If so it prints a one-line warning to
 * stderr.  Non-fatal: the runtime keeps running.
 *
 * SW_DEADLOCK_DETECT=0   — disable entirely (default: enabled)
 * SW_DEADLOCK_MS=<n>     — override wake interval in milliseconds
 */

/* === Process States === */
typedef enum {
    SW_PROC_FREE = 0,       /* Slot available */
    SW_PROC_RUNNABLE,       /* Ready to run */
    SW_PROC_RUNNING,        /* Currently executing */
    SW_PROC_WAITING,        /* Blocked on receive */
    SW_PROC_SUSPENDED,      /* Explicitly suspended */
    SW_PROC_EXITING,        /* Cleaning up */
    SW_PROC_GARBING,        /* GC in progress */
} sw_proc_state_t;

/* === Process Priorities === */
typedef enum {
    SW_PRIO_MAX = 0,
    SW_PRIO_HIGH = 1,
    SW_PRIO_NORMAL = 2,
    SW_PRIO_LOW = 3,
    SW_PRIO_NUM = 4
} sw_priority_t;

/* === Forward Declarations === */
struct sw_process;
struct sw_scheduler;
struct sw_swarm;
struct sw_value_arena;   /* GC v1 per-process value arena (swarmrt_varena.h) */
struct sw_val;           /* generated error slot value (swarmrt_lang.h) */
typedef struct sw_process sw_process_t;
typedef struct sw_scheduler sw_scheduler_t;
typedef struct sw_swarm sw_swarm_t;

/* === Generated-code execution state (PER PROCESS, not thread-local) ===
 * The compiled backend keeps a current source line/file (for panic banners)
 * and a call-stack ring buffer (for stack traces). These were scheduler-thread
 * locals, but a blocking op (sleep/receive) yields the fiber, so a process
 * resuming on the same scheduler thread would inherit an UNRELATED process's
 * line/file/trace — a panic would then print the wrong location/call chain.
 * (Memory-safe — they hold only .rodata string literals + ints — but wrong.)
 * So this state lives per-process: `_sw_gen` is swapped to the running process's
 * block at every context switch, and the generated `_sw_current_line` /
 * `_sw_current_file` / `_sw_trace*` macros (swarmrt_builtins_studio.h) redirect
 * through it. `_sw_frame_t` is shared with the generated trace push/pop. */
typedef struct { const char *module_name; const char *fn_name; int line; } _sw_frame_t;
typedef struct sw_gen_exec {
    int current_line;            /* source line of the statement now executing */
    const char *current_file;    /* source file (string literal, never freed)  */
    _sw_frame_t trace[64];       /* call-stack ring buffer (innermost last)     */
    int trace_top;               /* depth; >64 sets trace_overflowed            */
    int trace_overflowed;        /* 1 if the call chain exceeded 64 frames      */
} sw_gen_exec_t;
/* Points at the running process's gen_exec block (or a thread-local fallback for
 * non-process / OOM contexts). Swapped wherever tls_current is set. */
extern __thread sw_gen_exec_t *_sw_gen;

/* === Context (for assembly switching) === */
typedef struct {
#ifdef __aarch64__
    /* ARM64 callee-saved registers */
    uint64_t x19, x20, x21, x22, x23, x24;
    uint64_t x25, x26, x27, x28;
    uint64_t fp;    /* x29 */
    uint64_t lr;    /* x30 */
    uint64_t sp;
    uint64_t pc;
#else
    /* x86_64 callee-saved registers */
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;   /* Stack pointer */
    uint64_t rip;   /* Instruction pointer */
#endif
    /* For stack overflow checks */
    uint64_t stack_limit;
    uint64_t stack_base;
} sw_context_t;

/* Ownership v2: payload kind discriminant. Do NOT steal tag bits — tags are
 * load-bearing small ints (selective receive). RAW = the generic void* payload
 * (signals via deliver_signal, gen_server call structs, port structs — freed by
 * the existing paths). VALUE = an sw_val_t* graph OWNED by `region`, adopted into
 * the receiver's arena on match and bulk-freed (not free(payload)) on teardown. */
#define SW_PK_RAW   0
#define SW_PK_VALUE 1

/* === Message === */
typedef struct sw_msg {
    uint64_t tag;             /* Message tag for selective receive */
    void *payload;
    uint64_t from_pid;
    struct sw_msg *next;      /* Private queue: next in doubly-linked list */
    struct sw_msg *prev;      /* Private queue: prev in doubly-linked list */
    _Atomic(struct sw_msg *) sig_next;  /* Signal queue: Vyukov MPSC link */
    struct sw_value_arena *region;  /* v2: owns a SW_PK_VALUE payload while queued */
    uint8_t pkind;            /* v2: SW_PK_RAW (0) or SW_PK_VALUE (1) */
} sw_msg_t;

/* === Mailbox (Lock-free MPSC + process-local private queue) ===
 *
 * Two-part design:
 * 1. Signal stack: lock-free LIFO push (CAS), atomic steal for drain
 * 2. Private queue: process-local doubly-linked FIFO (for selective receive)
 *
 * Send path: CAS push to signal stack (completely lock-free).
 * Receive: atomic-steal signal stack, reverse to FIFO, append to private queue.
 * No sentinel nodes, no use-after-free, no contention on receive.
 */
typedef struct {
    /* Signal stack: lock-free LIFO (producers CAS-push here) */
    _Atomic(sw_msg_t *) sig_head;

    /* Private queue: process-local doubly-linked FIFO */
    sw_msg_t *priv_head;
    sw_msg_t *priv_tail;

    /* Wake-up coordination */
    _Atomic int waiting;

    uint32_t count;
} sw_mailbox_t;

/* === Heap (bump pointer allocator) === */
typedef struct {
    uint64_t *start;      /* Heap start */
    uint64_t *top;        /* Next free slot (bump pointer) */
    uint64_t *end;        /* Heap end */
    size_t size;          /* In words */
    
    /* Generational GC */
    uint64_t *old_heap;
    uint64_t *old_top;
    size_t old_size;
    uint32_t gen_gcs;
    uint8_t arena_backed;     /* 1 if start points into arena (don't free) */
} sw_heap_t;

/* === Stack (growable) === */
typedef struct {
    uint8_t *base;        /* Stack base (high address) */
    uint8_t *limit;       /* Stack limit (low address) */
    size_t size;          /* Current size */
    size_t max_size;      /* Max before expansion */
} sw_stack_t;

/* === Message Tags === */
#define SW_TAG_NONE    0
#define SW_TAG_EXIT    1      /* {EXIT, pid, reason} from linked process */
#define SW_TAG_DOWN    2      /* {DOWN, ref, pid, reason} from monitor */
#define SW_TAG_TIMER   3      /* Timer fired */
#define SW_TAG_CALL    10     /* GenServer synchronous call */
#define SW_TAG_CAST    11     /* GenServer async cast */
#define SW_TAG_STOP    12     /* GenServer stop request */
#define SW_TAG_TASK_RESULT 13 /* Task result from child */

/* === Link (bidirectional, intrusive list) === */
typedef struct sw_link {
    sw_process_t *peer;
    struct sw_link *next;
} sw_link_t;

/* === Monitor === */
typedef struct sw_monitor {
    uint64_t ref;
    sw_process_t *watcher;
    sw_process_t *watched;
    struct sw_monitor *next_in_watcher;  /* In watcher's my_monitors list */
    struct sw_monitor *next_in_watched;  /* In watched's monitors_me list */
} sw_monitor_t;

/* === Exit/DOWN signal payload ===
 *
 * `reason` is the legacy integer (-1 = panic, 0 = normal, other = user).
 * `reason_str` carries the human-readable message ("hd: list is empty",
 * "panic: foo", etc.) for trap_exit handlers that want to log or
 * dispatch on it. NULL when there's no string available (normal exit,
 * monitor DOWN for a process that exited cleanly). The codegen receive
 * loop synthesises {'EXIT', from, reason_str-or-int} so user patterns
 * see the message text instead of an opaque -1.
 *
 * Owned by the signal — deliver_signal strdups, msg_free frees. */
typedef struct {
    uint64_t pid;
    uint64_t ref;
    int reason;
    char *reason_str;
} sw_signal_t;

/* === Process Registry === */
#define SW_REGISTRY_BUCKETS 4096
#define SW_REG_NAME_MAX     64

typedef struct sw_reg_entry {
    char name[SW_REG_NAME_MAX];
    sw_process_t *proc;
    struct sw_reg_entry *next;
} sw_reg_entry_t;

typedef struct {
    sw_reg_entry_t **buckets;
    uint32_t num_buckets;
    pthread_rwlock_t lock;
} sw_registry_t;

/* === Timer === */
typedef struct sw_timer {
    uint64_t ref;
    uint64_t fire_at_ns;
    sw_process_t *dest;
    uint64_t tag;
    void *msg;
    struct sw_timer *next;
} sw_timer_t;

typedef struct {
    sw_timer_t *head;         /* Sorted by fire_at_ns */
    pthread_mutex_t lock;
} sw_timer_list_t;

/* === Process Control Block (THE key structure) === */
struct sw_process {
    /* === Frequently accessed (cache line 0) === */
    sw_heap_t heap;           /* Heap - bump allocation */
    uint64_t *htop;           /* Heap top pointer (cached) */
    uint64_t *stop;           /* Stack top (grows down) */
    int32_t fcalls;           /* Reductions remaining */
    uint32_t flags;           /* Process flags */
    
    /* === Scheduling (cache line 1) === */
    /* _Atomic: the scheduler thread writes state on every swap-in/out
     * while OTHER threads read it cross-thread (sw_monitor's
     * already-dead check, sw_send_after, the watchdog scanner) — TSan
     * flagged the unsynchronized accesses. Implicit accesses are seq_cst,
     * which is correct; the only hot writer is the scheduler swap-in.
     * Size/alignment of the enum are unchanged, so the asm-pinned ctx
     * offset (0x70) is preserved — the _Static_assert in the .c verifies. */
    _Atomic sw_proc_state_t state;
    sw_priority_t priority;
    sw_scheduler_t *scheduler;/* Current scheduler */
    uint64_t pid;
    
    /* === Context (for preemption) === */
    sw_context_t ctx;         /* Saved registers */
    void (*entry)(void*);     /* Entry function */
    void *arg;                /* Entry argument */

    /* === Process stack (for context switching) === */
    void *stack_mem;          /* malloc'd stack for this process */
    size_t stack_size;        /* Stack allocation size */

    /* === Arena allocation === */
    uint32_t arena_slot;      /* Index into proc_slab */
    uint32_t heap_block_idx;  /* Index into block pool */

    /* === GC v1: per-process VALUE arena ===
     * All sw_val_t this process builds while running live here; freed
     * wholesale in process_destroy. NULL until process_init_arena creates
     * it (and for the interpreter, which has no scheduler). Distinct from
     * `heap` above (the dead bump-block). See [[swarmrt-gc-design]]. */
    struct sw_value_arena *varena;

    /* === Run queue link (MPSC intrusive) === */
    _Atomic(sw_process_t *) rq_next;  /* Atomic for lock-free MPSC push */
    
    /* === Message passing === */
    sw_mailbox_t mailbox;
    
    /* === Statistics === */
    uint64_t reductions_done;
    uint64_t context_switches;
    uint64_t messages_sent;
    uint64_t messages_recv;
    
    /* === Links & Monitors === */
    sw_process_t *parent;
    sw_link_t *links;                  /* Bidirectional link list */
    sw_monitor_t *monitors_me;         /* Others monitoring this process */
    sw_monitor_t *my_monitors;         /* Monitors this process created */
    _Atomic int kill_flag;             /* Set by exit signal propagation (cross-thread;
                                          was volatile — not synchronization) */
    _Atomic int exit_reason;           /* Why this process exited. User/runtime code may
                                          write it directly (sw_self()->exit_reason = N)
                                          while monitor/process_exit read it — atomic. */
    char *panic_msg;                   /* Human-readable panic message,
                                          set by sw_process_panic, read by
                                          process_exit -> deliver_signal so
                                          {'EXIT', from, MSG_STR} reaches
                                          trap_exit handlers. malloc'd;
                                          freed in process_destroy UNDER link_lock
                                          (serialises with a late sw_monitor read).
                                          NULL if not a panic exit. */
    _Atomic(sw_reg_entry_t *) reg_entry; /* Registry entry (or NULL). Atomic: a child's
                                          process_exit reads it while sw_register (from
                                          sup_start_child on another thread) writes it. */
    void *ets_tables;                  /* Linked list of owned ETS tables */

    /* === ABA / arena slot-reuse defense ===
     *
     * `generation` is bumped on every process_init_arena. The scheduler
     * samples it at pick time and verifies it again before swapping in;
     * if the slot was reused in between, the picked process is stale and
     * we skip it.
     *
     * `ctx_lock` serialises the ctx write in process_init_arena against
     * the ctx copy in sw_safe_swap_into. Once copied to a local struct,
     * the asm swap reads from the local copy — process_init_arena can no
     * longer race with the asm reads (which used to be the crash path
     * documented in R2-#4 / R3-C / R4-B). */
    _Atomic uint64_t generation;
    sw_spinlock_t ctx_lock;

    /* GC v2: a SPAWN region handed to this process at spawn time, owning its
     * args/captures until the child's trampoline ADOPTS it into varena. Recorded
     * here (before the child is runnable) so a kill BEFORE the trampoline runs
     * still lets process_destroy reclaim it. NULL once adopted. Placed LAST so it
     * shifts no field offset (the asm + slot-reuse paths are offset-sensitive). */
    struct sw_value_arena *spawn_region;

    /* Generated-code error slot, PER PROCESS (not thread-local). The generated
     * `error(x)` writes here and `try/catch` reads/clears it via the _sw_error
     * macro -> sw_self_error_slot(). Per-process because the alternative — a
     * scheduler-thread-local — leaks across the context switch a blocking op
     * performs: a process resuming from sleep()/receive() inside a try would
     * otherwise catch an UNRELATED process's error, whose value (in that
     * process's arena) is freed when it exits -> use-after-free. The value lives
     * in THIS process's arena, so it's reclaimed with the arena on exit. (Placed
     * at the END so it shifts no asm-pinned offset.) */
    struct sw_val *gen_error;

    /* Per-process generated execution state (line/file/call-trace). Lazily
     * allocated on first run and KEPT WITH THE SLAB SLOT across lifetimes (like
     * stack_mem) — reset, not freed, in process_init_arena; freed only at slab
     * teardown. `_sw_gen` is pointed here on context-switch in. NULL until first
     * use / on OOM (then `_sw_gen` falls back to a thread-local). At struct END
     * so it shifts no asm-pinned offset. */
    sw_gen_exec_t *gen_exec;

    /* Optional per-process teardown hook, invoked in process_destroy on EVERY
     * exit path — normal return, kill, AND panic (a panic sw_context_swaps to
     * the scheduler and never returns to the entry fn, so a fiber-tail free
     * would be skipped; process_destroy always runs). Used by supervisor child
     * fibers to free their private start-closure copy crash-safely. The hook
     * frees GLOBAL-heap state only (never the arena, freed separately). NULL =
     * no hook. At struct END so it shifts no asm-pinned offset. */
    void (*on_destroy)(void *);
    void *on_destroy_arg;

    /* Innermost live try/catch frame for the COMPILED path, PER PROCESS for
     * the same reason as gen_error: a fiber can yield (receive/sleep) inside a
     * try and resume on another scheduler thread, so a thread-local would
     * unwind the wrong process. The frame itself (jmp_buf + prev link) is a C
     * local in the generated function, i.e. it lives on this process's fiber
     * stack — setjmp/longjmp state is fiber-contained and survives scheduler
     * migration. error() longjmps to this frame (full dynamic unwind, callee
     * errors land in the nearest enclosing catch); with no frame it stays the
     * documented "silent, continue with nil". Panics NEVER use this chain —
     * they remain uncatchable and kill the process. NULL when no try is live.
     * At struct END so it shifts no asm-pinned offset. */
    void *try_chain;

    /* Pending-message count for the SW_MAILBOX_MAX depth cap: incremented by
     * every producer (sw_send / sw_send_tagged[_msg] / deliver_signal / timer
     * fires), decremented on every pop/remove, re-incremented at the
     * push-back-to-front race-recovery sites. Deliberately NOT inside
     * sw_mailbox_t — the mailbox sits mid-struct and growing it would shift
     * the offset-sensitive fields after it (slot-reuse paths; see the
     * spawn_region comment). int64 (not uint32) so an accounting bug shows up
     * as a visible negative instead of wrapping into permanent-drop mode.
     * Producer-side check is approximate by design (relaxed atomics).
     * At struct END so it shifts no asm-pinned offset. */
    _Atomic int64_t mb_len;

    /* Handoff barrier. 1 while a scheduler thread owns+executes — or is mid
     * context-swap-out of — this fiber; 0 once fully off-CPU with its saved ctx
     * AND its proc->scheduler read committed. A scheduler that reaches this proc
     * via the stealable overflow queue MUST acquire-observe on_cpu==0 before it
     * claims proc->scheduler (native.c CLAIM at scheduler_loop) or copies
     * proc->ctx — else it double-schedules a fiber still saving registers on
     * another thread (torn ctx -> garbage PC/SIGBUS; clobbered proc->scheduler
     * read at the receive swap-out sites native.c:~3875/3896/3904). SET relaxed
     * under ctx_lock AFTER the gen-check in sw_safe_swap_into; CLEARED release in
     * scheduler_loop once swap-out returns. Only meaningful for num_schedulers>1.
     * At struct END -> shifts no asm-pinned offset (ctx 0x70 / entry / arg all
     * precede mb_len). */
    _Atomic int on_cpu;

    /* Ping-pong locality: pid of the last process that woke THIS process from a
     * fiber send while itself about to go idle (mailbox_wake). When the same
     * waker repeats, the two are a back-and-forth A<->B pair and get collapsed
     * onto one scheduler (overflow spin-steal, no cross-thread condvar). A
     * one-shot waker never repeats, so long-lived procs (supervisors, fan-in
     * collectors) keep exact baseline home routing and are never migrated.
     * relaxed atomics: a stale read only mis-classifies one placement decision,
     * never a correctness issue. Only meaningful for num_schedulers>1. At struct
     * END -> shifts no asm-pinned offset. */
    _Atomic uint64_t last_waker;

#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
    /* ThreadSanitizer fiber handle (TSan builds ONLY — compiled out entirely
     * otherwise, so it shifts NO asm-pinned offset in shipping builds; and being
     * at struct END it shifts none even under TSan). A sw process is a fiber whose
     * registers/stack are swapped in raw asm TSan cannot see; with a handle here,
     * TSan models the process as a continuous fiber whose vector clock TRAVELS
     * WITH IT across OS-scheduler-thread migration (the overflow-queue steal path).
     * Created per lifetime in process_init_arena, destroyed in process_destroy.
     * For a scheduler's embedded sched_proc this instead holds the OS thread's own
     * fiber, captured via __tsan_get_current_fiber() at scheduler_main entry. See
     * the fiber-model comment block in swarmrt_native.c. */
    void *tsan_fiber;
#endif
};

/* === Run Queue (Vyukov MPSC — lock-free enqueue) === */
typedef struct {
    /* Per-priority Vyukov MPSC queues.
     * Push: atomic_exchange on tail + store to prev->rq_next. Lock-free.
     * Pop: read head->rq_next, advance head. Single-consumer only. */
    sw_process_t *heads[SW_PRIO_NUM];               /* Consumer-private */
    _Atomic(sw_process_t *) tails[SW_PRIO_NUM];     /* Producers + consumer */
    sw_process_t stubs[SW_PRIO_NUM];                 /* Sentinel nodes */

    /* Idle notification — only used when scheduler has no work.
     * Separated from queue ops so the hot path is 100% lock-free. */
    _Atomic int idle;
    pthread_mutex_t idle_lock;
    pthread_cond_t idle_cond;
} sw_runq_t;

/* === Scheduler (per OS thread) === */
struct sw_scheduler {
    uint32_t id;
    pthread_t thread;
    sw_swarm_t *swarm;
    
    /* Current process */
    sw_process_t *current;

    /* Scheduler context (for context switching with processes) */
    sw_process_t sched_proc;

    /* Run queue */
    sw_runq_t runq;
    
    /* Statistics */
    uint64_t context_switches;
    uint64_t steal_attempts;
    uint64_t reductions;
    volatile uint64_t loop_iters;    /* Debug: total scheduler loop iterations */
    volatile uint64_t procs_run;     /* Debug: processes executed */
    volatile uint64_t idle_waits;    /* Debug: times entered idle wait */

    /* State */
    /* _Atomic (was volatile): TSan-clean cross-thread flags. volatile is
     * not synchronization in C11 — main's readiness spin reads `active`
     * while the scheduler thread writes it, and sw_shutdown writes
     * `should_exit`/`running` while scheduler loops read them. Plain
     * loads/stores on _Atomic int are seq_cst; these are cold flags, the
     * cost is irrelevant. (sw_scheduler is NOT asm-offset-pinned — only
     * sw_process is — so the layout change is safe.) */
    _Atomic int active;
    _Atomic int should_exit;

};

/* === Swarm (the runtime) === */
struct sw_swarm {
    char name[32];
    uint32_t num_schedulers;
    _Atomic int running;   /* see active/should_exit note above */

    /* Arena allocator (replaces process table, free list, PID lock) */
    sw_arena_t arena;

    /* Schedulers */
    sw_scheduler_t **schedulers;

    /* Statistics */
    _Atomic uint64_t total_spawns;
    uint64_t total_reductions;
    _Atomic uint64_t total_sends;

    /* Configuration */
    uint32_t next_sched;  /* Round-robin counter */

    /* Process registry */
    sw_registry_t registry;

    /* Timers */
    sw_timer_list_t timers;

    /* Link/monitor graph lock */
    pthread_mutex_t link_lock;

    /* Global overflow run queue for work stealing.
     * When a scheduler's local queue grows too large, excess is pushed here.
     * Idle schedulers steal from here before sleeping. Mutex-protected
     * since the steal path is cold (only when scheduler has no work). */
    struct {
        sw_process_t *head;
        sw_process_t *tail;
        uint32_t count;
        pthread_mutex_t lock;
    } overflow_rq;

    /* Monotonic counters */
    _Atomic uint64_t next_monitor_ref;
    _Atomic uint64_t next_timer_ref;
};

/* === Assembly Context Switch === */
extern void sw_context_swap(sw_process_t *from, sw_process_t *to);

/* Release a sw_msg_t envelope back to its allocator (currently a
 * per-thread freelist with malloc/free fallback). Does NOT free the
 * payload — pattern bindings in the receive body alias into the
 * payload, and the body's return value may itself be one of those
 * bindings, so payload ownership is left to the caller. Without this
 * release, the codegen receive path used to leak the envelope on every
 * matched message, defeating the per-thread msg freelist (msg_alloc
 * always hit malloc) and stressing the glibc arena during spawn-storms
 * with high receive counts. */
void sw_msg_release(sw_msg_t *m);

/* Swap into a context captured in a caller-owned struct rather than read
 * straight from `to->ctx`. Use this on the scheduler -> process direction
 * after copying the destination ctx under proc->ctx_lock; the copy fixed the
 * historical swap-in / process_init_arena race tracked as R2-#4. Saves
 * from->ctx exactly like sw_context_swap. */
extern void sw_context_swap_from_copy(sw_process_t *from,
                                      const sw_context_t *to_ctx);
extern uint64_t sw_rdtsc(void);

/* === Public API === */

/* Swarm lifecycle */
int sw_init(const char *name, uint32_t num_schedulers);
void sw_shutdown(int swarm_id);

/* === Graceful shutdown (Phase 4) ===========================================
 * Orderly drain-with-deadline, then the EXISTING hard teardown (sw_shutdown).
 * Sequence: flip a global draining flag (observable via sw_is_draining() and
 * the swarm_stats() `draining` field, so /readyz can report NOT-ready and a
 * load balancer drains first) → poll until the run-queues + every mailbox
 * quiesce, up to the deadline → cancel pending timers → sw_shutdown (joins the
 * schedulers, fires on_destroy per process, frees the arena).
 *
 * MUST run on the main/embedder thread — it (via sw_shutdown) JOINS the
 * scheduler threads, so calling it from a scheduler fiber would self-join.
 * `deadline_ms` >= 0 is an explicit budget in milliseconds (0 = no drain
 * window); < 0 uses SW_SHUTDOWN_GRACE_MS (default 5000). Returns 0 if the node
 * quiesced within the deadline, 1 if the deadline forced teardown with work
 * still outstanding.
 *
 * DEADLINE SCOPE (precise): the step-2 drain POLL always returns by the
 * deadline. The step-4 hard-teardown JOIN is bounded only for cooperatively-
 * scheduled fibers — a fiber running compiled sw hits a reduction checkpoint
 * (sw_check_reds) and returns to its scheduler, which honours should_exit, so a
 * busy *sw* loop is bounded (the shutdown gate proves this). BUT a fiber
 * BLOCKED IN A C BUILTIN / SYSCALL (e.g. http_get / db_query against a slow or
 * hung peer) owns its scheduler OS thread, reaches no checkpoint, and the join
 * — and thus this call — can exceed the deadline until that syscall returns.
 * The operational bounds for that case are a SECOND SIGTERM/SIGINT (hard _exit,
 * see _sw_term_handler) and the external supervisor's own stop-timeout →
 * SIGKILL. See docs/DEPLOYMENT.md.
 *
 * DRAIN GUARANTEE: messages already queued in mailboxes are drained (the
 * fibers get to run and consume them) until quiescent or the deadline;
 * quiescence requires TWO consecutive clean scans so a message planted during a
 * single lock-free scan is not missed. Pending TIMERS are cancelled (a
 * heartbeat/interval timer is discarded on shutdown, not waited on). In-flight
 * internal sends are NOT rejected (rejecting them would break a gen_server
 * call/reply mid-drain and prevent quiescence). */
int sw_shutdown_graceful(int swarm_id, int deadline_ms);

/* Resolve the configured grace deadline: SW_SHUTDOWN_GRACE_MS (ms, clamped to
 * [0, 3600000]) or the 5000 ms default. */
int sw_shutdown_grace_ms(void);

/* 1 once sw_shutdown_graceful has begun draining. Surfaced in swarm_stats()
 * (`draining`) so a readiness probe can fail during the drain window. */
int sw_is_draining(void);

/* Install async-signal-safe SIGTERM + SIGINT handlers that request graceful
 * shutdown. The handler ONLY sets an atomic flag (a second signal hard-exits
 * via _exit, an operator escape hatch); the actual drain runs from the
 * main-thread wait loop (sw_wait_for_exit), never a scheduler thread. Call
 * ONLY from a program's own entry (swc run / a `swc build` binary) so a library
 * embedder keeps its own signal disposition. No-op when SW_NO_SIGNAL_SHUTDOWN
 * is set. Does NOT touch the crash (SIGSEGV/SIGBUS/SIGABRT) or preemption
 * (SIGALRM) handlers installed by sw_init. */
void sw_install_shutdown_signals(void);

/* 1 if a SIGTERM/SIGINT (or sw_request_shutdown) has requested graceful
 * shutdown. Polled by the main-thread wait loop. */
int sw_shutdown_requested(void);

/* Request graceful shutdown programmatically (sets the same flag the signal
 * handler does). Safe to call from any thread/fiber — it only stores an atomic;
 * the main-thread wait loop performs the actual off-scheduler drain. */
void sw_request_shutdown(void);

/* Block the calling (main/embedder) thread until *done_flag becomes nonzero OR
 * a shutdown has been requested, whichever first. `lock`/`cond` are the caller's
 * done-flag mutex/condvar (a normal completion signals `cond`, so this returns
 * immediately with zero added latency; a shutdown request is noticed within
 * ~50 ms). Returns 1 if it woke on the done flag (normal main() return), 0 on a
 * shutdown request — the caller then chooses sw_shutdown vs sw_shutdown_graceful. */
int sw_wait_for_exit(volatile int *done_flag, pthread_mutex_t *lock,
                     pthread_cond_t *cond);

/* Per-process usable stack size (bytes). 0 = built-in 128KB default. Set
 * BEFORE sw_init when running the tree-walking interpreter on a process fiber
 * (`swc run`), which needs a deep C stack. Does NOT affect the compiled path. */
extern size_t sw_proc_stack_size;

/* Process table lookup by PID */
sw_process_t *sw_find_by_pid(uint64_t pid);
/* Like sw_find_by_pid but also returns slots for EXITING/dead processes
 * (any slab slot whose .pid still equals `pid`). Used to reconstruct a
 * SW_VAL_PID for a process that has already exited (e.g. the pid carried
 * inside a {'DOWN',...}/{'EXIT',...} message) so it compares == the pid
 * spawn() handed out. Returns NULL only if the slot was recycled. */
sw_process_t *sw_find_by_pid_any(uint64_t pid);

/* Process management */
sw_process_t *sw_spawn(void (*func)(void*), void *arg);
sw_process_t *sw_spawn_opts(void (*func)(void*), void *arg, sw_priority_t prio);
void sw_yield(void);
void sw_exit(sw_process_t *proc);
void sw_process_kill(sw_process_t *proc, int reason);

/* Preemption control (called from assembly) */
void sw_preempt(void);
int sw_check_reds(void);

/* Message passing */
void sw_send(sw_process_t *to, void *msg);
void *sw_receive(uint64_t timeout_ms);
void *sw_receive_nowait(void);

/* Process info */
sw_process_t *sw_self(void);
uint64_t sw_getpid(void);
sw_proc_state_t sw_get_state(sw_process_t *proc);

/* Red-zone stack guard — recoverable stack overflow.
 *
 * Generated function prologues call this; when the running fiber's stack
 * pointer is within SW_STACK_RED_ZONE of the guard page, the codegen raises
 * a NORMAL sw panic (per-process death, EXIT propagation to links/monitors,
 * supervisor restart) instead of letting the next deep call hit the guard
 * page — which is a native SIGSEGV/SIGBUS that kills the whole OS process
 * and bypasses every fault-tolerance layer the runtime has.
 *
 * The red zone must leave room for the panic path itself (vsnprintf +
 * fprintf + trace print ≈ 4KB) plus the deepest C-side excursion a builtin
 * makes below an sw frame (to_string/format recursion is bounded by the
 * 253 value-depth cap ≈ 25KB worst case). 32KB covers both.
 *
 * Returns 0 when not on a fiber (interpreter/REPL/C threads — the
 * tree-walking interpreter has its own stack-near-limit guard), or when
 * the probe address is not inside this fiber's stack (delta > stack_size:
 * scheduler-thread code running with tls_current still set). */
#define SW_STACK_RED_ZONE (32 * 1024)
static inline int sw_stack_low(void) {
    sw_process_t *p = sw_self();
    if (!p || !p->stack_mem) return 0;
    char probe;
    uintptr_t delta = (uintptr_t)&probe - (uintptr_t)p->stack_mem;
    return delta <= p->stack_size && delta < SW_STACK_RED_ZONE;
}

/* Statistics */
void sw_stats(int swarm_id);
uint64_t sw_process_count(int swarm_id);

/* Links */
int sw_link(sw_process_t *other);
int sw_unlink(sw_process_t *other);
sw_process_t *sw_spawn_link(void (*func)(void*), void *arg);

/* Monitors */
uint64_t sw_monitor(sw_process_t *target);
int sw_demonitor(uint64_t ref);

/* Process flags */
void sw_process_flag(uint32_t flag, int value);
#define SW_FLAG_TRAP_EXIT  0x01

/* Registry */
int sw_register(const char *name, sw_process_t *proc);
int sw_unregister(const char *name);
sw_process_t *sw_whereis(const char *name);
int sw_send_named(const char *name, uint64_t tag, void *msg);

/* Timers */
uint64_t sw_send_after(uint64_t delay_ms, sw_process_t *dest, uint64_t tag, void *msg);
int sw_cancel_timer(uint64_t ref);

/* Tagged messages & selective receive */
void sw_send_tagged(sw_process_t *to, uint64_t tag, void *msg);
/* Ownership v2: enqueue an sw_val_t VALUE owned by `region` (SW_PK_VALUE). */
void sw_send_tagged_msg(sw_process_t *to, uint64_t tag, void *payload,
                        struct sw_value_arena *region);
void *sw_receive_tagged(uint64_t tag, uint64_t timeout_ms);
void *sw_receive_any(uint64_t timeout_ms, uint64_t *out_tag);
/* Ownership v2: sw_receive_any that adopts a VALUE region into the caller's
 * arena (caller incorporates the payload; must NOT free it). Used by pmap. */
void *sw_recv_any_adopt(uint64_t timeout_ms, uint64_t *out_tag);

/* Generated-code error slot for the current process (thread-local fallback when
 * outside a process). The generated `_sw_error` macro is (*sw_self_error_slot()).
 * Per-process so a try/catch resuming after a blocking op can't catch another
 * fiber's error (whose value would be freed on that fiber's exit). */
struct sw_val **sw_self_error_slot(void);

/* Innermost live compiled try/catch frame for the current process
 * (thread-local fallback outside a process). Same per-process rationale as
 * sw_self_error_slot; see the try_chain field comment in struct sw_process.
 * Holds a _sw_try_frame_t* (defined in the studio builtins header). */
void **sw_self_try_chain(void);

/* Ownership v2: spawn a process that OWNS `region` (its args/captures live in it)
 * until the child's trampoline adopts it. Reclaimed by process_destroy if the
 * child dies pre-trampoline; on spawn failure (NULL) the caller reclaims it. */
sw_process_t *sw_spawn_owned(void (*entry)(void*), void *arg, struct sw_value_arena *region);
/* Spawn with a per-process teardown hook recorded BEFORE the child is runnable
 * (proc->on_destroy = dtor, on_destroy_arg = arg), so even a pre-trampoline kill
 * reclaims `arg` via process_destroy. dtor(arg) frees the spawn arg. On spawn
 * failure (NULL) nothing was recorded — the caller reclaims `arg`. */
sw_process_t *sw_spawn_dtor(void (*entry)(void*), void *arg, void (*dtor)(void*));
sw_process_t *sw_spawn_link_dtor(void (*entry)(void*), void *arg, void (*dtor)(void*));
/* Called by the child trampoline: atomically take (read+clear) this process's
 * spawn_region so it can adopt it and process_destroy won't double-free. */
struct sw_value_arena *sw_self_take_spawn_region(void);

/* Total messages dropped by the SW_MAILBOX_MAX depth cap since boot
 * (monotonic, process-global). 0 when the cap is disabled or never hit.
 * Observability hook for tests/ops — see SW_MAILBOX_MAX_DEFAULT above. */
uint64_t sw_mailbox_dropped(void);

/* Total messages dropped by the SW_MSG_MAX_BYTES size cap since boot
 * (monotonic, process-global; 0 = cap disabled or never hit). The cap bounds
 * a single LOCAL value message's deep-copied size (the message region's
 * total_bytes); default 0 = unlimited. Oversized sends are dropped loudly
 * (rate-limited stderr), leak-free, with the receiver still woken. EXIT/DOWN
 * signals are exempt. Scope: region sends only — the SW_GC_OFF global-heap
 * fallback is not metered (same caveat as SW_PROC_MEM_MAX). */
uint64_t sw_msgsize_dropped(void);

/* Phase-4 observability counters (monotonic, process-global, _Atomic).
 * sw_proc_crashes: abnormal process exits (panic / uncaught error / kill —
 * any process_exit with a non-zero reason) since boot. sw_restarts_total:
 * supervisor child restarts (static supervise() + dynamic dyn_supervisor())
 * since boot; bumped by sw_note_restart() from the two restart-record choke
 * points (sup_record_restart / dynsup_record_restart). Surfaced to sw via
 * the swarm_stats() builtin. */
uint64_t sw_proc_crashes(void);
uint64_t sw_restarts_total(void);
void sw_note_restart(void);

/* Selective receive: scan mailbox without consuming, remove matched msg */
void sw_mailbox_drain_signals(void);         /* Drain signal stack → private queue */
sw_msg_t *sw_mailbox_peek(void);             /* First message in private queue (no pop) */
void sw_mailbox_remove_msg(sw_msg_t *m);     /* Remove specific message from queue */
int sw_mailbox_wait_new(uint64_t timeout_ms);/* Block until new message or timeout. Returns 1 if msg, 0 if timeout */

/* === Internal === */
sw_process_t *sw_schedule(sw_scheduler_t *sched);
void sw_add_to_runq(sw_runq_t *rq, sw_process_t *proc);
sw_process_t *sw_pick_next(sw_scheduler_t *sched);
sw_process_t *sw_steal_work(sw_scheduler_t *sched);
void sw_reschedule(sw_process_t *proc);

/* Arena management */
int sw_arena_init(sw_arena_t *arena, uint32_t max_procs);

/* Process trampoline (assembly entry point) */
extern void sw_process_trampoline(void);

/* Called by trampoline when entry() returns */
void sw_process_done(sw_process_t *proc);

/* sw_process_panic: abort the current process with a non-zero exit
 * reason and an optional human-readable message, then context-swap
 * back to the scheduler. The scheduler runs process_exit() which
 * delivers EXIT signals to linked processes (or kills them if they're
 * not trapping exits) and DOWN messages to monitors. Does not return.
 * `msg` is strdup'd onto the process so process_exit can hand it to
 * deliver_signal — pass NULL for "no useful message" (the int reason
 * is still propagated). Used by panic()/expect() + the codegen-emitted
 * runtime panics (hd of empty list, /0, etc.). */
void sw_process_panic(sw_process_t *proc, int reason, const char *msg);

#endif /* SWARMRT_NATIVE_H */
