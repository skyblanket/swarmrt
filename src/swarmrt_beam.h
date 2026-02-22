/*
 * SwarmRT-BEAM - Full BEAM Parity Implementation
 * 
 * Based on Erlang/OTP beam_emu.c, erl_process.c, erl_process.h
 * Goal: Match BEAM semantics exactly
 */

#ifndef SWARMRT_BEAM_H
#define SWARMRT_BEAM_H

#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <pthread.h>

/* === BEAM Constants === */
#define SWARM_BEAM_MAX_PROCESSES   (1 << 20)  /* 1M like BEAM */
#define SWARM_BEAM_MAX_SCHEDULERS  1024
#define SWARM_BEAM_CONTEXT_REDS    2000        /* BEAM's magic number */
#define SWARM_BEAM_MIN_CONTEXT_SWITCH_REDS 200 /* 10% of context reds */

/* Process heap starts at ~233 words (~1.8KB) like BEAM */
#define SWARM_BEAM_HEAP_MIN_SIZE   233
#define SWARM_BEAM_STACK_SIZE      (8 * 1024)  /* 8KB initial, grows */

/* === Forward Declarations === */
typedef struct sw_beam_process sw_beam_process_t;
typedef struct sw_beam_scheduler sw_beam_scheduler_t;
typedef struct sw_beam_swarm sw_beam_swarm_t;
typedef struct sw_beam_msg sw_beam_msg_t;
typedef struct sw_beam_term sw_beam_term_t;
typedef struct sw_beam_heap sw_beam_heap_t;

/* === Term Types (Erlang-compatible) === */
typedef enum {
    SW_BEAM_ATOM,
    SW_BEAM_INTEGER,
    SW_BEAM_FLOAT,
    SW_BEAM_PID,
    SW_BEAM_TUPLE,
    SW_BEAM_LIST,
    SW_BEAM_NIL,
    SW_BEAM_BINARY,
    SW_BEAM_REF,
    SW_BEAM_PORT,
    SW_BEAM_FUN
} sw_beam_term_type_t;

/* === Term Structure (boxed values like BEAM) === */
struct sw_beam_term {
    sw_beam_term_type_t type;
    union {
        int64_t i;
        double f;
        uint64_t pid;
        struct {
            char *data;
            size_t len;
        } atom;
        struct {
            sw_beam_term_t **items;
            uint32_t arity;
        } tuple;
        struct {
            sw_beam_term_t *head;
            sw_beam_term_t *tail;
        } list;
        struct {
            uint8_t *data;
            size_t size;
        } binary;
        struct {
            void (*func)(void*);
            void *arg;
        } fun;
    } val;
};

/* === Per-Process Heap (BEAM-style) === */
struct sw_beam_heap {
    sw_beam_term_t *start;      /* Heap start */
    sw_beam_term_t *top;        /* Heap top - next alloc */
    sw_beam_term_t *end;        /* Heap end */
    size_t size;                /* Current size in words */
    
    /* Generational GC support */
    sw_beam_term_t *old_heap;   /* Old generation */
    sw_beam_term_t *old_top;
    size_t old_size;
    uint16_t gen_gcs;           /* Number of minor GCs */
};

/* === Process States (BEAM-compatible) === */
typedef enum {
    SW_BEAM_PROC_RUNNING = 0,    /* Currently executing */
    SW_BEAM_PROC_RUNNABLE,       /* Ready to run */
    SW_BEAM_PROC_WAITING,        /* Waiting for message */
    SW_BEAM_PROC_EXITING,        /* Dying */
    SW_BEAM_PROC_GARBING         /* Doing GC */
} sw_beam_proc_state_t;

/* === Process Flags === */
#define SW_BEAM_F_TRAP_EXIT      (1 << 0)
#define SW_BEAM_F_EXITING        (1 << 1)
#define SW_BEAM_F_ACTIVE         (1 << 2)
#define SW_BEAM_F_GC_PENDING     (1 << 3)

/* === Message Structure (BEAM-style copying) === */
struct sw_beam_msg {
    sw_beam_term_t *payload;     /* Copied to receiver's heap */
    sw_beam_process_t *from;     /* Sender */
    uint64_t timestamp;
    struct sw_beam_msg *next;
    struct sw_beam_msg *prev;
};

/* === Message Queue === */
typedef struct {
    sw_beam_msg_t *first;        /* Head of queue */
    sw_beam_msg_t *last;         /* Tail of queue */
    uint32_t count;
    pthread_mutex_t lock;
} sw_beam_msgq_t;

/* === Process Control Block (BEAM-style) ===
 * 
 * Frequent fields first for cache locality (like BEAM does)
 */
struct sw_beam_process {
    /* === FREQUENTLY ACCESSED (cache line 0) === */
    sw_beam_term_t *htop;        /* Heap top - where next alloc goes */
    sw_beam_term_t *stop;        /* Stack top (grows downward) */

    uint32_t freason;            /* Failure reason */
    sw_beam_term_t *fvalue;      /* Exit/throw value */

    int32_t fcalls;              /* REDUCTIONS LEFT - THE MAGIC! */
    uint32_t flags;              /* Trap exit, etc */

    /* === HEAP INFO (cache line 1) === */
    sw_beam_heap_t heap;

    /* === SCHEDULING (cache line 2) === */
    pthread_mutex_t proc_lock;   /* Protects state + scheduler assignment */
    sw_beam_proc_state_t state;
    uint64_t pid;
    uint32_t priority;           /* 0=max, 3=low */

    sw_beam_scheduler_t *scheduler;
    uint64_t reductions_done;    /* Total reductions executed */
    
    /* Run queue linkage */
    sw_beam_process_t *rq_next;
    sw_beam_process_t *rq_prev;
    
    /* Process table linkage */
    sw_beam_process_t *pt_next;
    
    /* === MESSAGE QUEUE === */
    sw_beam_msgq_t mailbox;
    sw_beam_msg_t *saved_last;   /* For selective receive */
    
    /* === LINKS/MONITORS === */
    sw_beam_process_t *links;
    sw_beam_process_t *link_next;
    sw_beam_process_t *parent;   /* For supervision */
    
    /* === CONTEXT SWITCHING === */
    /* Use setjmp/longjmp for now, can upgrade to asm later */
    jmp_buf env;
    void (*entry)(void*);
    void *arg;
    
    /* === SUPERVISION === */
    uint32_t restart_count;
    uint64_t last_restart;
    
    /* === REGISTERS (for context switch) === */
    sw_beam_term_t *x_regs[16];  /* X0-X15 argument registers */
    uint32_t arity;              /* Number of live args */
};

/* === Run Queue (per scheduler) === */
typedef struct {
    sw_beam_process_t *head;
    sw_beam_process_t *tail;
    uint32_t count;
    
    /* Priority queues (BEAM has 4: max, high, normal, low) */
    sw_beam_process_t *prio_heads[4];
    sw_beam_process_t *prio_tails[4];
    uint32_t prio_counts[4];
    
    pthread_mutex_t lock;
    pthread_cond_t nonempty;
    uint32_t flags;
} sw_beam_runq_t;

/* === Scheduler (OS Thread) === */
struct sw_beam_scheduler {
    uint32_t id;
    pthread_t thread;
    sw_beam_runq_t runq;
    sw_beam_process_t *current;
    sw_beam_swarm_t *swarm;
    
    /* Stats */
    uint64_t context_switches;
    uint64_t reductions_done;
    
    /* Work stealing */
    volatile int active;
    uint32_t steal_attempts;
    
    /* Scheduler-local storage (like BEAM's esdp) */
    sw_beam_term_t *x_reg_array[16];
};

/* === Swarm (Collection of Schedulers) === */
struct sw_beam_swarm {
    char name[32];
    uint32_t num_schedulers;
    sw_beam_scheduler_t *schedulers[SWARM_BEAM_MAX_SCHEDULERS];
    
    /* Process table (hash table would be better) */
    sw_beam_process_t **process_table;
    pthread_rwlock_t table_lock;  /* Protects process table */
    uint64_t next_pid;
    pthread_mutex_t pid_lock;
    uint64_t total_processes;
    
    /* Global stats */
    uint64_t total_spawns;
    uint64_t total_reductions;
    uint64_t total_sends;
    
    /* For load balancing */
    uint32_t next_sched;
    
    volatile int running;
};

/* === Public API === */

/* System */
int swarm_beam_init(const char *name, uint32_t num_schedulers);
void swarm_beam_shutdown(int swarm_id);
sw_beam_swarm_t *swarm_beam_get(int swarm_id);

/* Process Management */
sw_beam_process_t *sw_beam_spawn(void (*func)(void*), void *arg);
sw_beam_process_t *sw_beam_spawn_on(int swarm_id, uint32_t sched_id, 
                                     void (*func)(void*), void *arg);
void sw_beam_exit(sw_beam_process_t *proc, int reason);
void sw_beam_link(sw_beam_process_t *p1, sw_beam_process_t *p2);

/* THE CORE: Scheduler (erts_schedule equivalent) */
sw_beam_process_t *sw_beam_schedule(sw_beam_scheduler_t *sched, 
                                     sw_beam_process_t *proc, 
                                     int reds_used);

/* Scheduling Primitives */
void sw_beam_yield(void);
void sw_beam_schedule_me(sw_beam_process_t *proc);
void sw_beam_wait(sw_beam_process_t *proc);
void sw_beam_wake(sw_beam_process_t *proc);

/* Reduction counting (PREEMPTIVE SCHEDULING) */
#define SW_BEAM_CHECK_REDUCTIONS(proc) \
    do { \
        if (ERTS_UNLIKELY(--(proc)->fcalls <= 0)) { \
            sw_beam_yield(); \
        } \
    } while(0)

#define ERTS_UNLIKELY(x) __builtin_expect((x), 0)

/* Message Passing (COPYING - like BEAM) */
void sw_beam_send(sw_beam_process_t *to, sw_beam_term_t *msg);
sw_beam_term_t *sw_beam_receive(uint64_t timeout_ms);
sw_beam_term_t *sw_beam_receive_selective(
    bool (*matcher)(sw_beam_term_t*, void*),
    void *pattern,
    uint64_t timeout_ms
);

/* Heap Operations */
sw_beam_term_t *sw_beam_heap_alloc(sw_beam_process_t *proc, size_t words);
sw_beam_term_t *sw_beam_copy_term(sw_beam_process_t *to, sw_beam_term_t *term);
void sw_beam_gc(sw_beam_process_t *proc);

/* Term Construction */
sw_beam_term_t *sw_beam_mk_atom(const char *name);
sw_beam_term_t *sw_beam_mk_int(sw_beam_process_t *proc, int64_t val);
sw_beam_term_t *sw_beam_mk_pid(uint64_t pid);
sw_beam_term_t *sw_beam_mk_tuple(sw_beam_process_t *proc, uint32_t arity, ...);
sw_beam_term_t *sw_beam_mk_list(sw_beam_process_t *proc, sw_beam_term_t *head, sw_beam_term_t *tail);

/* Context Switching (Assembly-level) */
void sw_beam_context_save(sw_beam_process_t *p);
void sw_beam_context_restore(sw_beam_process_t *p);
void sw_beam_context_swap(sw_beam_process_t *from, sw_beam_process_t *to);

/* Work Stealing */
sw_beam_process_t *sw_beam_steal_work(sw_beam_scheduler_t *sched);

/* Stats */
void swarm_beam_stats(int swarm_id);

#endif /* SWARMRT_BEAM_H */
