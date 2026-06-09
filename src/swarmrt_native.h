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
    sw_proc_state_t state;
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
    volatile int kill_flag;            /* Set by exit signal propagation */
    int exit_reason;                   /* Why this process exited (legacy int) */
    char *panic_msg;                   /* Human-readable panic message,
                                          set by sw_process_panic, read by
                                          process_exit -> deliver_signal so
                                          {'EXIT', from, MSG_STR} reaches
                                          trap_exit handlers. malloc'd;
                                          freed in process_destroy. NULL
                                          if not a panic exit. */
    sw_reg_entry_t *reg_entry;         /* Registry entry (or NULL) */
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
    volatile int active;
    volatile int should_exit;

};

/* === Swarm (the runtime) === */
struct sw_swarm {
    char name[32];
    uint32_t num_schedulers;
    volatile int running;

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
