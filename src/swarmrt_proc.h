/*
 * SwarmRT Process Subsystem
 *
 * Per-process heaps, reduction-counted preemptive scheduling,
 * copying message passing, work-stealing run queues.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_PROC_H
#define SWARMRT_PROC_H

#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <pthread.h>

/* === Constants === */
#define SWARM_PROC_MAX_PROCESSES   (1 << 20)  /* 1M processes */
#define SWARM_PROC_MAX_SCHEDULERS  1024
#define SWARM_PROC_CONTEXT_REDS    2000        /* reductions per time slice */
#define SWARM_PROC_MIN_CONTEXT_SWITCH_REDS 200 /* 10% of context reds */

/* Process heap starts at ~233 words (~1.8KB) */
#define SWARM_PROC_HEAP_MIN_SIZE   233
#define SWARM_PROC_STACK_SIZE      (8 * 1024)  /* 8KB initial, grows */

/* === Forward Declarations === */
typedef struct sw_proc_process sw_proc_process_t;
typedef struct sw_proc_scheduler sw_proc_scheduler_t;
typedef struct sw_proc_swarm sw_proc_swarm_t;
typedef struct sw_proc_msg sw_proc_msg_t;
typedef struct sw_proc_term sw_proc_term_t;
typedef struct sw_proc_heap sw_proc_heap_t;

/* === Term Types === */
typedef enum {
    SW_PROC_ATOM,
    SW_PROC_INTEGER,
    SW_PROC_FLOAT,
    SW_PROC_PID,
    SW_PROC_TUPLE,
    SW_PROC_LIST,
    SW_PROC_NIL,
    SW_PROC_BINARY,
    SW_PROC_REF,
    SW_PROC_PORT,
    SW_PROC_FUN
} sw_proc_term_type_t;

/* === Term Structure (boxed values) === */
struct sw_proc_term {
    sw_proc_term_type_t type;
    union {
        int64_t i;
        double f;
        uint64_t pid;
        struct {
            char *data;
            size_t len;
        } atom;
        struct {
            sw_proc_term_t **items;
            uint32_t arity;
        } tuple;
        struct {
            sw_proc_term_t *head;
            sw_proc_term_t *tail;
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

/* === Per-Process Heap === */
struct sw_proc_heap {
    sw_proc_term_t *start;      /* Heap start */
    sw_proc_term_t *top;        /* Heap top - next alloc */
    sw_proc_term_t *end;        /* Heap end */
    size_t size;                /* Current size in words */
    
    /* Generational GC support */
    sw_proc_term_t *old_heap;   /* Old generation */
    sw_proc_term_t *old_top;
    size_t old_size;
    uint16_t gen_gcs;           /* Number of minor GCs */
};

/* === Process States === */
typedef enum {
    SW_PROC_PROC_RUNNING = 0,    /* Currently executing */
    SW_PROC_PROC_RUNNABLE,       /* Ready to run */
    SW_PROC_PROC_WAITING,        /* Waiting for message */
    SW_PROC_PROC_EXITING,        /* Dying */
    SW_PROC_PROC_GARBING         /* Doing GC */
} sw_proc_proc_state_t;

/* === Process Flags === */
#define SW_PROC_F_TRAP_EXIT      (1 << 0)
#define SW_PROC_F_EXITING        (1 << 1)
#define SW_PROC_F_ACTIVE         (1 << 2)
#define SW_PROC_F_GC_PENDING     (1 << 3)

/* === Message Structure (copy-on-send) === */
struct sw_proc_msg {
    sw_proc_term_t *payload;     /* Copied to receiver's heap */
    sw_proc_process_t *from;     /* Sender */
    uint64_t timestamp;
    struct sw_proc_msg *next;
    struct sw_proc_msg *prev;
};

/* === Message Queue === */
typedef struct {
    sw_proc_msg_t *first;        /* Head of queue */
    sw_proc_msg_t *last;         /* Tail of queue */
    uint32_t count;
    pthread_mutex_t lock;
} sw_proc_msgq_t;

/* === Process Control Block ===
 *
 * Frequent fields first for cache locality
 */
struct sw_proc_process {
    /* === FREQUENTLY ACCESSED (cache line 0) === */
    sw_proc_term_t *htop;        /* Heap top - where next alloc goes */
    sw_proc_term_t *stop;        /* Stack top (grows downward) */

    uint32_t freason;            /* Failure reason */
    sw_proc_term_t *fvalue;      /* Exit/throw value */

    int32_t fcalls;              /* REDUCTIONS LEFT - THE MAGIC! */
    uint32_t flags;              /* Trap exit, etc */

    /* === HEAP INFO (cache line 1) === */
    sw_proc_heap_t heap;

    /* === SCHEDULING (cache line 2) === */
    pthread_mutex_t proc_lock;   /* Protects state + scheduler assignment */
    sw_proc_proc_state_t state;
    uint64_t pid;
    uint32_t priority;           /* 0=max, 3=low */

    sw_proc_scheduler_t *scheduler;
    uint64_t reductions_done;    /* Total reductions executed */
    
    /* Run queue linkage */
    sw_proc_process_t *rq_next;
    sw_proc_process_t *rq_prev;
    
    /* Process table linkage */
    sw_proc_process_t *pt_next;
    
    /* === MESSAGE QUEUE === */
    sw_proc_msgq_t mailbox;
    sw_proc_msg_t *saved_last;   /* For selective receive */
    
    /* === LINKS/MONITORS === */
    sw_proc_process_t *links;
    sw_proc_process_t *link_next;
    sw_proc_process_t *parent;   /* For supervision */
    
    /* === CONTEXT SWITCHING === */
    /* Use setjmp/longjmp for now, can upgrade to asm later */
    jmp_buf env;
    void (*entry)(void*);
    void *arg;
    
    /* === SUPERVISION === */
    uint32_t restart_count;
    uint64_t last_restart;
    
    /* === REGISTERS (for context switch) === */
    sw_proc_term_t *x_regs[16];  /* X0-X15 argument registers */
    uint32_t arity;              /* Number of live args */
};

/* === Run Queue (per scheduler) === */
typedef struct {
    sw_proc_process_t *head;
    sw_proc_process_t *tail;
    uint32_t count;
    
    /* Priority queues: max, high, normal, low */
    sw_proc_process_t *prio_heads[4];
    sw_proc_process_t *prio_tails[4];
    uint32_t prio_counts[4];
    
    pthread_mutex_t lock;
    pthread_cond_t nonempty;
    uint32_t flags;
} sw_proc_runq_t;

/* === Scheduler (OS Thread) === */
struct sw_proc_scheduler {
    uint32_t id;
    pthread_t thread;
    sw_proc_runq_t runq;
    sw_proc_process_t *current;
    sw_proc_swarm_t *swarm;
    
    /* Stats */
    uint64_t context_switches;
    uint64_t reductions_done;
    
    /* Work stealing */
    volatile int active;
    uint32_t steal_attempts;
    
    /* Scheduler-local storage */
    sw_proc_term_t *x_reg_array[16];
};

/* === Swarm (Collection of Schedulers) === */
struct sw_proc_swarm {
    char name[32];
    uint32_t num_schedulers;
    sw_proc_scheduler_t *schedulers[SWARM_PROC_MAX_SCHEDULERS];
    
    /* Process table (hash table would be better) */
    sw_proc_process_t **process_table;
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
int swarm_proc_init(const char *name, uint32_t num_schedulers);
void swarm_proc_shutdown(int swarm_id);
sw_proc_swarm_t *swarm_proc_get(int swarm_id);

/* Process Management */
sw_proc_process_t *sw_proc_spawn(void (*func)(void*), void *arg);
sw_proc_process_t *sw_proc_spawn_on(int swarm_id, uint32_t sched_id, 
                                     void (*func)(void*), void *arg);
void sw_proc_exit(sw_proc_process_t *proc, int reason);
void sw_proc_link(sw_proc_process_t *p1, sw_proc_process_t *p2);

/* Scheduler core */
sw_proc_process_t *sw_proc_schedule(sw_proc_scheduler_t *sched, 
                                     sw_proc_process_t *proc, 
                                     int reds_used);

/* Scheduling Primitives */
void sw_proc_yield(void);
void sw_proc_schedule_me(sw_proc_process_t *proc);
void sw_proc_wait(sw_proc_process_t *proc);
void sw_proc_wake(sw_proc_process_t *proc);

/* Reduction counting (PREEMPTIVE SCHEDULING) */
#define SW_PROC_CHECK_REDUCTIONS(proc) \
    do { \
        if (SW_UNLIKELY(--(proc)->fcalls <= 0)) { \
            sw_proc_yield(); \
        } \
    } while(0)

#define SW_UNLIKELY(x) __builtin_expect((x), 0)

/* Message Passing (copy-on-send) */
void sw_proc_send(sw_proc_process_t *to, sw_proc_term_t *msg);
sw_proc_term_t *sw_proc_receive(uint64_t timeout_ms);
sw_proc_term_t *sw_proc_receive_selective(
    bool (*matcher)(sw_proc_term_t*, void*),
    void *pattern,
    uint64_t timeout_ms
);

/* Heap Operations */
sw_proc_term_t *sw_proc_heap_alloc(sw_proc_process_t *proc, size_t words);
sw_proc_term_t *sw_proc_copy_term(sw_proc_process_t *to, sw_proc_term_t *term);
void sw_proc_gc(sw_proc_process_t *proc);

/* Term Construction */
sw_proc_term_t *sw_proc_mk_atom(const char *name);
sw_proc_term_t *sw_proc_mk_int(sw_proc_process_t *proc, int64_t val);
sw_proc_term_t *sw_proc_mk_pid(uint64_t pid);
sw_proc_term_t *sw_proc_mk_tuple(sw_proc_process_t *proc, uint32_t arity, ...);
sw_proc_term_t *sw_proc_mk_list(sw_proc_process_t *proc, sw_proc_term_t *head, sw_proc_term_t *tail);

/* Context Switching (Assembly-level) */
void sw_proc_context_save(sw_proc_process_t *p);
void sw_proc_context_restore(sw_proc_process_t *p);
void sw_proc_context_swap(sw_proc_process_t *from, sw_proc_process_t *to);

/* Work Stealing */
sw_proc_process_t *sw_proc_steal_work(sw_proc_scheduler_t *sched);

/* Stats */
void swarm_proc_stats(int swarm_id);

#endif /* SWARMRT_PROC_H */
