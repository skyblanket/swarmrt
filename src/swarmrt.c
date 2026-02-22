/*
 * SwarmRT - Minimal Actor Runtime in C
 * Built for AI-agent coordination
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include "swarmrt.h"

/* === Global State === */
static sw_swarm_t g_swarm;
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

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

/* === Scheduler Thread === */
static void *scheduler_loop(void *arg) {
    sw_scheduler_t *sched = (sw_scheduler_t *)arg;
    
    while (sched->running) {
        pthread_mutex_lock(&sched->run_queue.lock);
        sw_process_t *proc = sched->run_queue.head;
        
        if (proc) {
            /* Dequeue */
            sched->run_queue.head = proc->next;
            if (sched->run_queue.head) {
                sched->run_queue.head->prev = NULL;
            } else {
                sched->run_queue.tail = NULL;
            }
            sched->run_queue.count--;
            pthread_mutex_unlock(&sched->run_queue.lock);
            
            /* Run process */
            sched->current = proc;
            proc->state = SW_RUNNING;
            proc->reductions = 0;
            
            /* Context switch to process */
            if (setjmp(sched->current->context) == 0) {
                longjmp(proc->context, 1);
            }
            
            sched->current = NULL;
        } else {
            pthread_mutex_unlock(&sched->run_queue.lock);
            usleep(1000); /* 1ms idle */
        }
    }
    
    return NULL;
}

/* === Process Entry Wrapper === */
static void process_entry(void *arg) {
    sw_process_t *proc = (sw_process_t *)arg;
    
    /* Save context for scheduler */
    if (setjmp(proc->context) == 0) {
        /* First entry - yield to scheduler */
        return;
    }
    
    /* Actually run */
    proc->entry(proc->arg);
    proc->state = SW_EXITED;
    sw_yield();
}

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
    
    /* Create schedulers */
    for (uint32_t i = 0; i < num_schedulers; i++) {
        sw_scheduler_t *sched = calloc(1, sizeof(sw_scheduler_t));
        sched->id = i;
        sched->running = true;
        pthread_mutex_init(&sched->run_queue.lock, NULL);
        g_swarm.schedulers[i] = sched;
        
        pthread_create(&sched->thread, NULL, scheduler_loop, sched);
    }
    
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
    
    for (uint32_t i = 0; i < g_swarm.num_schedulers; i++) {
        sw_scheduler_t *sched = g_swarm.schedulers[i];
        sched->running = false;
        pthread_join(sched->thread, NULL);
        free(sched);
    }
    
    g_initialized = 0;
    pthread_mutex_unlock(&g_init_lock);
    
    printf("[SwarmRT] Shutdown complete\n");
}

sw_process_t *sw_spawn(void (*func)(void *), void *arg) {
    static uint32_t next_sched = 0;
    uint32_t sched_id = __sync_fetch_and_add(&next_sched, 1) % g_swarm.num_schedulers;
    return sw_spawn_on(sched_id, func, arg);
}

sw_process_t *sw_spawn_on(uint32_t scheduler_id, void (*func)(void *), void *arg) {
    sw_process_t *proc = calloc(1, sizeof(sw_process_t));
    
    pthread_mutex_lock(&g_swarm.pid_lock);
    proc->pid = ++g_swarm.next_pid;
    pthread_mutex_unlock(&g_swarm.pid_lock);
    
    proc->state = SW_RUNNING;
    proc->entry = func;
    proc->arg = arg;
    proc->stack = malloc(SWARM_STACK_SIZE);
    proc->stack_size = SWARM_STACK_SIZE;
    
    pthread_mutex_init(&proc->mailbox.lock, NULL);
    
    /* Get scheduler */
    sw_scheduler_t *sched = g_swarm.schedulers[scheduler_id % g_swarm.num_schedulers];
    proc->scheduler = sched;
    
    /* Set up initial context */
    if (setjmp(proc->context) == 0) {
        /* Initialize stack and entry point */
        process_entry(proc);
    }
    
    /* Add to run queue */
    pthread_mutex_lock(&sched->run_queue.lock);
    proc->next = NULL;
    proc->prev = sched->run_queue.tail;
    if (sched->run_queue.tail) {
        sched->run_queue.tail->next = proc;
    } else {
        sched->run_queue.head = proc;
    }
    sched->run_queue.tail = proc;
    sched->run_queue.count++;
    pthread_mutex_unlock(&sched->run_queue.lock);
    
    return proc;
}

void sw_yield(void) {
    sw_scheduler_t *sched = g_swarm.schedulers[0]; /* TODO: get current */
    
    /* Save context and return to scheduler */
    if (setjmp(sched->current->context) == 0) {
        longjmp(sched->current->context, 1);
    }
}

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
    
    /* Wake up if waiting */
    if (to->state == SW_WAITING) {
        to->state = SW_RUNNING;
        sw_schedule(to);
    }
}

sw_term_t *sw_receive(uint64_t timeout_ms) {
    sw_scheduler_t *sched = g_swarm.schedulers[0]; /* TODO */
    sw_process_t *proc = sched->current;
    
    while (1) {
        pthread_mutex_lock(&proc->mailbox.lock);
        sw_msg_t *msg = proc->mailbox.head;
        
        if (msg) {
            proc->mailbox.head = msg->next;
            if (!proc->mailbox.head) {
                proc->mailbox.tail = NULL;
            }
            proc->mailbox.count--;
            pthread_mutex_unlock(&proc->mailbox.lock);
            
            sw_term_t *payload = msg->payload;
            free(msg);
            return payload;
        }
        
        pthread_mutex_unlock(&proc->mailbox.lock);
        
        /* No message - wait or timeout */
        if (timeout_ms == 0) {
            proc->state = SW_WAITING;
            sw_yield();
            proc->state = SW_RUNNING;
        } else {
            /* TODO: timeout handling */
            usleep(1000);
        }
    }
}

void sw_schedule(sw_process_t *proc) {
    sw_scheduler_t *sched = proc->scheduler;
    
    pthread_mutex_lock(&sched->run_queue.lock);
    proc->next = NULL;
    proc->prev = sched->run_queue.tail;
    if (sched->run_queue.tail) {
        sched->run_queue.tail->next = proc;
    } else {
        sched->run_queue.head = proc;
    }
    sched->run_queue.tail = proc;
    sched->run_queue.count++;
    pthread_mutex_unlock(&sched->run_queue.lock);
}

/* === Term Constructors === */
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
    /* TODO: va_arg */
    return t;
}

/* === Swarm Primitives === */
typedef struct {
    sw_map_func_t func;
    sw_term_t *item;
    sw_term_t *result;
    int done;
} pmap_worker_t;

static void pmap_worker(void *arg) {
    pmap_worker_t *w = (pmap_worker_t *)arg;
    w->result = w->func(w->item);
    w->done = 1;
}

sw_term_t **swarm_pmap(sw_map_func_t func, sw_term_t **items, uint32_t count) {
    pmap_worker_t *workers = malloc(sizeof(pmap_worker_t) * count);
    sw_process_t **procs = malloc(sizeof(sw_process_t *) * count);
    
    /* Spawn workers */
    for (uint32_t i = 0; i < count; i++) {
        workers[i].func = func;
        workers[i].item = items[i];
        workers[i].done = 0;
        procs[i] = sw_spawn(pmap_worker, &workers[i]);
    }
    
    /* Collect results */
    sw_term_t **results = malloc(sizeof(sw_term_t *) * count);
    for (uint32_t i = 0; i < count; i++) {
        while (!workers[i].done) {
            usleep(100);
        }
        results[i] = workers[i].result;
    }
    
    free(workers);
    free(procs);
    return results;
}

/* === Debug === */
void swarm_stats(void) {
    printf("\n=== SwarmRT Stats ===\n");
    printf("Schedulers: %d\n", g_swarm.num_schedulers);
    printf("Next PID: %llu\n", g_swarm.next_pid);
    
    for (uint32_t i = 0; i < g_swarm.num_schedulers; i++) {
        sw_scheduler_t *sched = g_swarm.schedulers[i];
        printf("  Scheduler %d: %d processes, %llu reductions\n",
               i, sched->run_queue.count, sched->reductions_done);
    }
    printf("====================\n\n");
}
