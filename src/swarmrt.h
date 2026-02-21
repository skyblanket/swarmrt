#ifndef SWARMRT_H
#define SWARMRT_H

#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <pthread.h>

/* === Configuration === */
#define SWARM_MAX_PROCESSES 100000
#define SWARM_MAX_SCHEDULERS 64
#define SWARM_MSG_QUEUE_SIZE 1024
#define SWARM_STACK_SIZE (64 * 1024)  /* 64KB per process */
#define SWARM_REDUCTIONS_PER_SLICE 2000

/* === Forward Declarations === */
typedef struct sw_process sw_process_t;
typedef struct sw_scheduler sw_scheduler_t;
typedef struct sw_msg sw_msg_t;
typedef struct sw_term sw_term_t;

/* === Term Types (Erlang-like) === */
typedef enum {
    SW_ATOM,
    SW_INTEGER,
    SW_FLOAT,
    SW_PID,
    SW_TUPLE,
    SW_LIST,
    SW_BINARY,
    SW_REF
} sw_term_type_t;

/* === Process State === */
typedef enum {
    SW_RUNNING,
    SW_WAITING,      /* Waiting for message */
    SW_SUSPENDED,    /* I/O wait */
    SW_EXITED
} sw_proc_state_t;

/* === Message Structure === */
struct sw_msg {
    sw_term_t *payload;
    sw_process_t *from;
    sw_msg_t *next;
};

/* === Message Queue === */
typedef struct {
    sw_msg_t *head;
    sw_msg_t *tail;
    uint32_t count;
    pthread_mutex_t lock;
} sw_msg_queue_t;

/* === Process Control Block === */
struct sw_process {
    uint64_t pid;
    sw_proc_state_t state;
    
    /* Context switching */
    jmp_buf context;
    uint8_t *stack;
    uint64_t stack_size;
    
    /* Mailbox */
    sw_msg_queue_t mailbox;
    
    /* Scheduling */
    sw_scheduler_t *scheduler;
    uint32_t reductions;
    uint64_t priority;  /* Lower = higher priority */
    
    /* Linked processes (for exit signals) */
    sw_process_t *links;
    sw_process_t *link_next;
    
    /* Supervision */
    sw_process_t *parent;
    uint32_t restart_count;
    uint64_t last_restart;
    
    /* Function entry point */
    void (*entry)(void *arg);
    void *arg;
    
    /* Thread (simple version) */
    pthread_t thread;
    
    /* Next process in scheduler queue */
    sw_process_t *next;
    sw_process_t *prev;
};

/* === Run Queue === */
typedef struct {
    sw_process_t *head;
    sw_process_t *tail;
    uint32_t count;
    pthread_mutex_t lock;
} sw_run_queue_t;

/* === Scheduler (OS Thread) === */
struct sw_scheduler {
    uint32_t id;
    pthread_t thread;
    sw_run_queue_t run_queue;
    sw_process_t *current;
    bool running;
    uint64_t reductions_done;
};

/* === Swarm Scheduler === */
typedef struct {
    sw_scheduler_t *schedulers[SWARM_MAX_SCHEDULERS];
    uint32_t num_schedulers;
    uint64_t next_pid;
    pthread_mutex_t pid_lock;
} sw_swarm_t;

/* === Supervisor Spec === */
typedef struct {
    const char *name;
    void (*start_func)(void);
    uint32_t restart_policy;  /* permanent, temporary, transient */
    uint32_t max_restarts;
    uint64_t restart_window;  /* milliseconds */
} sw_sup_spec_t;

/* === Public API === */

/* System */
int swarm_init(uint32_t num_schedulers);
void swarm_shutdown(void);
void swarm_stats(void);

/* Process Management */
sw_process_t *sw_spawn(void (*func)(void *), void *arg);
sw_process_t *sw_spawn_on(uint32_t scheduler_id, void (*func)(void *), void *arg);
void sw_exit(sw_process_t *proc, int reason);
void sw_link(sw_process_t *p1, sw_process_t *p2);
void sw_unlink(sw_process_t *p1, sw_process_t *p2);

/* Message Passing */
void sw_send(sw_process_t *to, sw_term_t *msg);
sw_term_t *sw_receive(uint64_t timeout_ms);

/* Scheduling */
void sw_yield(void);
void sw_schedule(sw_process_t *proc);

/* Supervisors */
int sw_sup_start_link(sw_sup_spec_t *specs, uint32_t count);

/* Swarm Primitives */
typedef sw_term_t *(*sw_map_func_t)(sw_term_t *);
sw_term_t **swarm_map(sw_map_func_t func, sw_term_t **items, uint32_t count);
sw_term_t **swarm_pmap(sw_map_func_t func, sw_term_t **items, uint32_t count);

/* Term Construction (simplified) */
sw_term_t *sw_mk_atom(const char *name);
sw_term_t *sw_mk_int(int64_t val);
sw_term_t *sw_mk_pid(uint64_t pid);
sw_term_t *sw_mk_tuple(uint32_t size, ...);
sw_term_t *sw_mk_list(uint32_t count, ...);

/* Context Switching */
void sw_context_switch(sw_process_t *from, sw_process_t *to);

#endif /* SWARMRT_H */
