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
typedef struct sw_process sw_process_t;
typedef struct sw_scheduler sw_scheduler_t;
typedef struct sw_swarm sw_swarm_t;

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

/* === Message === */
typedef struct sw_msg {
    uint64_t tag;             /* Message tag for selective receive */
    void *payload;
    uint64_t from_pid;
    struct sw_msg *next;      /* Private queue: next in doubly-linked list */
    struct sw_msg *prev;      /* Private queue: prev in doubly-linked list */
    _Atomic(struct sw_msg *) sig_next;  /* Signal queue: Vyukov MPSC link */
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

/* === Exit/DOWN signal payload === */
typedef struct {
    uint64_t pid;
    uint64_t ref;
    int reason;
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
    int exit_reason;                   /* Why this process exited */
    sw_reg_entry_t *reg_entry;         /* Registry entry (or NULL) */
    void *ets_tables;                  /* Linked list of owned ETS tables */

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

    /* Monotonic counters */
    _Atomic uint64_t next_monitor_ref;
    _Atomic uint64_t next_timer_ref;
};

/* === Assembly Context Switch === */
extern void sw_context_swap(sw_process_t *from, sw_process_t *to);
extern uint64_t sw_rdtsc(void);

/* === Public API === */

/* Swarm lifecycle */
int sw_init(const char *name, uint32_t num_schedulers);
void sw_shutdown(int swarm_id);

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
void *sw_receive_tagged(uint64_t tag, uint64_t timeout_ms);
void *sw_receive_any(uint64_t timeout_ms, uint64_t *out_tag);

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

#endif /* SWARMRT_NATIVE_H */
