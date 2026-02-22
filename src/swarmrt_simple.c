/*
 * SwarmRT - Simplified Working Runtime
 * Focus on getting processes actually running
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include "swarmrt.h"

/* === Global State === */
static sw_swarm_t g_swarm;
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

/* === Simple Worker Thread Approach === */

typedef struct {
    sw_process_t *proc;
    pthread_t thread;
} worker_ctx_t;

static void *simple_worker(void *arg) {
    sw_process_t *proc = (sw_process_t *)arg;
    
    proc->state = SW_RUNNING;
    proc->entry(proc->arg);
    proc->state = SW_EXITED;
    
    return NULL;
}

/* === Term Implementation === */
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

int swarm_init(uint32_t num_schedulers) {
    pthread_mutex_lock(&g_init_lock);
    
    if (g_initialized) {
        pthread_mutex_unlock(&g_init_lock);
        return 0;
    }
    
    memset(&g_swarm, 0, sizeof(g_swarm));
    g_swarm.num_schedulers = num_schedulers;
    pthread_mutex_init(&g_swarm.pid_lock, NULL);
    
    g_initialized = 1;
    pthread_mutex_unlock(&g_init_lock);
    
    printf("[SwarmRT] Initialized with %d scheduler(s)\n", num_schedulers);
    return 0;
}

void swarm_shutdown(void) {
    pthread_mutex_lock(&g_init_lock);
    
    if (!g_initialized) {
        pthread_mutex_unlock(&g_init_lock);
        return;
    }
    
    /* Note: In real implementation, we'd track all threads and join them */
    
    g_initialized = 0;
    pthread_mutex_unlock(&g_init_lock);
    
    printf("[SwarmRT] Shutdown complete\n");
}

sw_process_t *sw_spawn(void (*func)(void *), void *arg) {
    return sw_spawn_on(0, func, arg);
}

sw_process_t *sw_spawn_on(uint32_t scheduler_id, void (*func)(void *), void *arg) {
    (void)scheduler_id;
    
    sw_process_t *proc = calloc(1, sizeof(sw_process_t));
    
    pthread_mutex_lock(&g_swarm.pid_lock);
    proc->pid = ++g_swarm.next_pid;
    pthread_mutex_unlock(&g_swarm.pid_lock);
    
    proc->state = SW_RUNNING;
    proc->entry = func;
    proc->arg = arg;
    proc->stack = NULL; /* Using pthread stack */
    proc->stack_size = 0;
    
    pthread_mutex_init(&proc->mailbox.lock, NULL);
    
    /* Create pthread for this process */
    pthread_create(&proc->thread, NULL, simple_worker, proc);
    
    return proc;
}

void sw_exit(sw_process_t *proc, int reason) {
    (void)reason;
    proc->state = SW_EXITED;
    pthread_exit(NULL);
}

void sw_link(sw_process_t *p1, sw_process_t *p2) {
    (void)p1;
    (void)p2;
    /* TODO: implement linking */
}

void sw_unlink(sw_process_t *p1, sw_process_t *p2) {
    (void)p1;
    (void)p2;
}

/* Message Passing */
void sw_send(sw_process_t *to, sw_term_t *msg) {
    sw_msg_t *m = malloc(sizeof(sw_msg_t));
    m->payload = msg;
    m->from = NULL; /* TODO: current process */
    m->next = NULL;
    
    pthread_mutex_lock(&to->mailbox.lock);
    if (to->mailbox.tail) {
        to->mailbox.tail->next = m;
    } else {
        to->mailbox.head = m;
    }
    to->mailbox.tail = m;
    to->mailbox.count++;
    pthread_mutex_unlock(&to->mailbox.lock);
}

sw_term_t *sw_receive(uint64_t timeout_ms) {
    (void)timeout_ms;
    /* TODO: implement with condition variable */
    return NULL;
}

/* Scheduling - NO-OP in simple version */
void sw_yield(void) {
    #ifdef __APPLE__
        pthread_yield_np();
    #else
        pthread_yield();
    #endif
}

void sw_schedule(sw_process_t *proc) {
    (void)proc;
}

/* Supervisors */
int sw_sup_start_link(sw_sup_spec_t *specs, uint32_t count) {
    (void)specs;
    (void)count;
    return 0;
}

/* Term Constructors */
sw_term_t *sw_mk_atom(const char *name) {
    sw_term_t *t = malloc(sizeof(sw_term_t));
    t->type = SW_ATOM;
    t->val.atom = strdup(name);
    return t;
}

sw_term_t *sw_mk_int(int64_t val) {
    sw_term_t *t = malloc(sizeof(sw_term_t));
    t->type = SW_INTEGER;
    t->val.i = val;
    return t;
}

sw_term_t *sw_mk_pid(uint64_t pid) {
    sw_term_t *t = malloc(sizeof(sw_term_t));
    t->type = SW_PID;
    t->val.pid = pid;
    return t;
}

sw_term_t *sw_mk_tuple(uint32_t size, ...) {
    sw_term_t *t = malloc(sizeof(sw_term_t));
    t->type = SW_TUPLE;
    t->val.tuple.count = size;
    t->val.tuple.items = malloc(sizeof(sw_term_t *) * size);
    (void)size; /* TODO: va_arg */
    return t;
}

/* Swarm Primitives */
sw_term_t **swarm_pmap(sw_map_func_t func, sw_term_t **items, uint32_t count) {
    sw_process_t **procs = malloc(sizeof(sw_process_t *) * count);
    worker_ctx_t *ctx = malloc(sizeof(worker_ctx_t) * count);
    
    /* Spawn workers */
    for (uint32_t i = 0; i < count; i++) {
        ctx[i].proc = calloc(1, sizeof(sw_process_t));
        ctx[i].proc->pid = i + 1;
        ctx[i].proc->entry = (void (*)(void *))func;
        ctx[i].proc->arg = items[i];
        
        pthread_create(&ctx[i].thread, NULL, simple_worker, ctx[i].proc);
        procs[i] = ctx[i].proc;
    }
    
    /* Collect results - in real version, use message passing */
    sw_term_t **results = malloc(sizeof(sw_term_t *) * count);
    
    for (uint32_t i = 0; i < count; i++) {
        pthread_join(ctx[i].thread, NULL);
        /* In real version, result would be in message */
        results[i] = NULL;
        free(ctx[i].proc);
    }
    
    free(procs);
    free(ctx);
    return results;
}

/* Debug */
void swarm_stats(void) {
    printf("\n=== SwarmRT Stats ===\n");
    printf("Schedulers: %d\n", g_swarm.num_schedulers);
    printf("Next PID: %llu\n", (unsigned long long)g_swarm.next_pid);
    printf("====================\n\n");
}
