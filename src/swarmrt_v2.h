/*
 * SwarmRT v2 - Full User-Space Threading
 * M:N threading: M user processes on N OS threads
 * Goal: sub-microsecond spawn, ~100-300ns context switch
 */

#ifndef SWARMRT_V2_H
#define SWARMRT_V2_H

#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <pthread.h>
#include <signal.h>

/* === Configuration === */
#define SWARM_MAX_SCHEDULERS       64
#define SWARM_MAX_PROCESSES        1000000    /* 1M processes */
#define SWARM_STACK_SIZE           1024       /* 1KB initial stack (not 64KB!) */
#define SWARM_REDUCTIONS           2000       /* reductions per time slice */
#define SWARM_MAX_SWARMS           16

/* === Forward Declarations === */
typedef struct sw_process sw_process_t;
typedef struct sw_scheduler sw_scheduler_t;
typedef struct sw_swarm sw_swarm_t;
typedef struct sw_msg sw_msg_t;
typedef struct sw_term sw_term_t;
typedef struct sw_context sw_context_t;

/* === Process States === */
typedef enum {
    SW_PROC_RUNNING,      /* Currently executing */
    SW_PROC_RUNNABLE,     /* Ready to run */
    SW_PROC_WAITING,      /* Waiting for message */
    SW_PROC_SUSPENDED,    /* I/O wait */
    SW_PROC_EXITED        /* Dead */
} sw_proc_state_t;

/* === Stack Structure (growable) === */
typedef struct {
    uint8_t *base;        /* Stack base (lowest address) */
    uint8_t *top;         /* Current stack pointer */
    size_t size;          /* Current size */
    size_t max_size;      /* Max before gc/expand */
} sw_stack_t;

/* === Process Context (for setjmp/longjmp) === */
struct sw_context {
    jmp_buf env;          /* Saved registers */
    sw_stack_t stack;     /* Process stack */
    void (*func)(void*);  /* Entry function */
    void *arg;            /* Argument */
    int reductions;       /* Remaining reductions */
};

/* === Message Queue === */
typedef struct sw_msgq {
    sw_msg_t *head;
    sw_msg_t *tail;
    uint32_t count;
    pthread_mutex_t lock;
} sw_msgq_t;

/* === Process Control Block === */
struct sw_process {
    uint64_t pid;
    sw_proc_state_t state;
    
    /* Execution context */
    sw_context_t ctx;
    
    /* Program counter (for bytecode) */
    uint8_t *pc;
    
    /* Heap (simplified for now) */
    uint8_t *heap;
    uint8_t *htop;
    size_t heap_size;
    
    /* Mailbox */
    sw_msgq_t mailbox;
    
    /* Scheduling */
    sw_scheduler_t *scheduler;
    int priority;
    uint64_t total_reductions;
    
    /* Linked list in run queue */
    sw_process_t *rq_next;
    sw_process_t *rq_prev;
    
    /* Linked list in process table */
    sw_process_t *pt_next;
    
    /* For supervisor */
    sw_process_t *parent;
    uint32_t restart_count;
    
    /* Flags */
    uint32_t flags;
};

/* === Run Queue (per scheduler) === */
typedef struct {
    sw_process_t *head;
    sw_process_t *tail;
    uint32_t count;
    pthread_mutex_t lock;
    pthread_cond_t nonempty;
} sw_runq_t;

/* === Scheduler (OS Thread) === */
struct sw_scheduler {
    uint32_t id;
    pthread_t thread;
    sw_runq_t runq;
    sw_process_t *current;
    sw_swarm_t *swarm;
    
    /* Stats */
    uint64_t context_switches;
    uint64_t reductions_done;
    
    /* For work stealing */
    volatile int active;
    int steal_attempts;
};

/* === Swarm (Collection of Schedulers) === */
struct sw_swarm {
    char name[32];
    uint32_t num_schedulers;
    sw_scheduler_t *schedulers[SWARM_MAX_SCHEDULERS];
    
    /* Process table */
    sw_process_t *processes;
    uint64_t next_pid;
    pthread_mutex_t pid_lock;
    uint64_t total_processes;
    
    /* Global stats */
    uint64_t total_spawns;
    uint64_t total_reductions;
    
    /* For load balancing */
    uint32_t next_sched;
};

/* === Message Structure === */
struct sw_msg {
    sw_term_t *payload;
    sw_process_t *from;
    uint64_t timestamp;
    sw_msg_t *next;
};

/* === Term Types === */
typedef enum {
    SW_ATOM,
    SW_INTEGER,
    SW_FLOAT,
    SW_PID,
    SW_TUPLE,
    SW_LIST,
    SW_BINARY,
    SW_REF,
    SW_PORT,
    SW_FUN
} sw_term_type_t;

struct sw_term {
    sw_term_type_t type;
    union {
        int64_t i;
        double f;
        uint64_t pid;
        struct {
            char *data;
            size_t len;
        } binary;
        struct {
            sw_term_t **items;
            uint32_t count;
        } tuple;
        struct {
            sw_term_t *head;
            sw_term_t *tail;
        } list;
        char *atom;
    } val;
};

/* === Public API === */

/* Swarm Management */
int swarm_v2_init(const char *name, uint32_t num_schedulers);
void swarm_v2_shutdown(int swarm_id);
sw_swarm_t *swarm_v2_get(int swarm_id);

/* Process Management */
sw_process_t *sw_v2_spawn(void (*func)(void*), void *arg);
sw_process_t *sw_v2_spawn_on(int swarm_id, uint32_t sched_id, void (*func)(void*), void *arg);
void sw_v2_yield(void);
void sw_v2_exit(int reason);

/* Scheduling */
void sw_v2_schedule_me(void);
sw_process_t *sw_v2_pick_next(sw_scheduler_t *sched);

/* Message Passing */
void sw_v2_send(sw_process_t *to, sw_term_t *msg);
sw_term_t *sw_v2_receive(uint64_t timeout_ms);

/* Context Switching (the magic) */
void sw_v2_context_save(sw_process_t *p);
void sw_v2_context_restore(sw_process_t *p);
void sw_v2_context_swap(sw_process_t *from, sw_process_t *to);

/* Stack Management */
int sw_v2_stack_init(sw_stack_t *stack, size_t size);
void sw_v2_stack_free(sw_stack_t *stack);
int sw_v2_stack_grow(sw_stack_t *stack);

/* Stats */
void swarm_v2_stats(int swarm_id);

/* === Swarm Primitives === */
typedef sw_term_t *(*sw_map_func_t)(sw_term_t *);
sw_term_t **sw_v2_pmap(sw_map_func_t func, sw_term_t **items, uint32_t count, int max_workers);

#endif
