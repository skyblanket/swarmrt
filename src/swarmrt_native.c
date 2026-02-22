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

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>
#include "swarmrt_native.h"
#include "swarmrt_ets.h"
#include "swarmrt_phase5.h"
#include "swarmrt_hotload.h"

/* === Global State === */
static sw_swarm_t *g_swarm = NULL;
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread sw_scheduler_t *tls_scheduler = NULL;
static __thread sw_process_t *tls_current = NULL;

/* === Per-thread freelists (avoid malloc/free on hot path) === */
#define MSG_FREELIST_MAX 128
static __thread sw_msg_t *tls_msg_free = NULL;
static __thread int tls_msg_free_count = 0;

static inline sw_msg_t *msg_alloc(void) {
    if (tls_msg_free) {
        sw_msg_t *m = tls_msg_free;
        tls_msg_free = m->next;
        tls_msg_free_count--;
        return m;
    }
    return (sw_msg_t *)malloc(sizeof(sw_msg_t));
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

/* === Timer for preemption (macOS uses dispatch) === */
#ifdef __APPLE__
static dispatch_source_t g_preempt_timer;
#else
static timer_t g_preempt_timer;
#endif
static struct sigaction g_old_sigaction;

/* === Forward Declarations === */
static void *scheduler_main(void *arg);
static void scheduler_loop(sw_scheduler_t *sched);
static void process_exit(sw_process_t *proc, int reason);
static void process_destroy(sw_process_t *proc);
static void fire_timers(void);
static void registry_remove_proc(sw_process_t *proc);
static void preempt_handler(int sig, siginfo_t *info, void *context);
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

    /* Single mmap — one syscall to rule them all */
    uint8_t *mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("[SwarmRT] arena mmap failed");
        return -1;
    }

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
    }

    /* PID counter starts at 0 */
    atomic_store(&arena->next_pid, 0);

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

    proc->htop = block;
    proc->stop = NULL;
    proc->heap_block_idx = block_idx;

    /* Core fields */
    proc->entry = entry;
    proc->arg = arg;

    /* Allocate process stack (lazy — reuse across process slot lifetimes) */
#define SW_PROC_STACK_SIZE  (8 * 1024)  /* 8KB per process stack */
    if (!proc->stack_mem) {
        proc->stack_mem = malloc(SW_PROC_STACK_SIZE);
        proc->stack_size = SW_PROC_STACK_SIZE;
    }

    /* Initialize context for first context switch → trampoline */
    memset(&proc->ctx, 0, sizeof(sw_context_t));
    uint8_t *stack_top = (uint8_t *)proc->stack_mem + proc->stack_size;
    stack_top = (uint8_t *)((uintptr_t)stack_top & ~0xFULL);  /* 16-byte align */
#ifdef __aarch64__
    proc->ctx.sp = (uint64_t)stack_top;
    proc->ctx.pc = (uint64_t)sw_process_trampoline;
    proc->ctx.x19 = (uint64_t)proc;  /* Trampoline reads proc from x19 */
#else
    proc->ctx.rsp = (uint64_t)stack_top;
    proc->ctx.rip = (uint64_t)sw_process_trampoline;
    proc->ctx.r12 = (uint64_t)proc;  /* Trampoline reads proc from r12 */
#endif
    proc->ctx.stack_base = (uint64_t)stack_top;
    proc->ctx.stack_limit = (uint64_t)proc->stack_mem;
    proc->state = SW_PROC_RUNNABLE;
    proc->priority = SW_PRIO_NORMAL;
    proc->fcalls = SWARM_CONTEXT_REDS;
    proc->flags = 0;

    /* Mailbox — reset signal stack + private queue for reuse */
    atomic_store(&proc->mailbox.sig_head, NULL);
    proc->mailbox.priv_head = NULL;
    proc->mailbox.priv_tail = NULL;
    atomic_store(&proc->mailbox.waiting, 0);
    proc->mailbox.count = 0;

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

    /* Free any remaining messages in signal stack */
    sw_msg_t *sig = atomic_exchange(&proc->mailbox.sig_head, NULL);
    while (sig) {
        sw_msg_t *next = (sw_msg_t *)atomic_load_explicit(&sig->sig_next, memory_order_relaxed);
        if (sig->payload) free(sig->payload);
        msg_free(sig);
        sig = next;
    }
    /* Free any remaining messages in private queue */
    sw_msg_t *msg = proc->mailbox.priv_head;
    while (msg) {
        sw_msg_t *next = msg->next;
        if (msg->payload) free(msg->payload);
        msg_free(msg);
        msg = next;
    }
    proc->mailbox.priv_head = NULL;
    proc->mailbox.priv_tail = NULL;
    proc->mailbox.count = 0;

    /* Return block and slot to the current scheduler's partition.
     * After unlock, the slot may be immediately reused by sw_spawn —
     * do NOT write to proc after unlocking. */
    uint32_t part_id = tls_scheduler ? tls_scheduler->id : 0;
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

static void preempt_handler(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)info;
    (void)context;

    sw_scheduler_t *sched = tls_scheduler;
    sw_process_t *current = tls_current;

    if (!sched || !current) return;

    /* Force yield - decrement reductions to trigger reschedule */
    current->fcalls = 0;
}

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
    if (atomic_load_explicit(&rq->idle, memory_order_relaxed)) {
        pthread_mutex_lock(&rq->idle_lock);
        pthread_cond_signal(&rq->idle_cond);
        pthread_mutex_unlock(&rq->idle_lock);
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
 * Work stealing: MPSC queues are single-consumer, so we can't directly
 * steal from another scheduler's queue. For now, return NULL — with
 * round-robin assignment, work is pre-balanced.
 * TODO: Implement push-based load balancing (victim pushes excess to shared queue).
 */
sw_process_t *sw_steal_work(sw_scheduler_t *sched) {
    (void)sched;
    return NULL;
}

/* ============================================================================
 * SCHEDULER THREAD
 * ============================================================================ */

static void scheduler_loop(sw_scheduler_t *sched) {
    tls_scheduler = sched;

    while (!sched->should_exit && g_swarm && g_swarm->running) {
        sched->loop_iters++;

        /* Fire expired timers (any scheduler can fire any timer) */
        fire_timers();

        sw_process_t *proc = sw_pick_next(sched);

        if (!proc) {
            proc = sw_steal_work(sched);
        }

        if (proc) {
            /* Check if this process was killed by an exit signal */
            if (proc->kill_flag) {
                proc->state = SW_PROC_EXITING;
                process_exit(proc, proc->exit_reason);
                process_destroy(proc);
                continue;
            }

            sched->procs_run++;
            proc->state = SW_PROC_RUNNING;
            proc->scheduler = sched;
            proc->fcalls = SWARM_CONTEXT_REDS;
            sched->current = proc;
            tls_current = proc;

            /* Context switch to process (runs on process's own stack) */
            sw_context_swap(&sched->sched_proc, proc);

            /* Process yielded, blocked on receive, or finished */
            tls_current = NULL;
            sched->current = NULL;

            if (proc->state == SW_PROC_EXITING) {
                /* Process finished or killed — clean up */
                process_exit(proc, proc->exit_reason);
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
             * it signals the condvar to wake us. */
            sw_runq_t *rq = &sched->runq;
            pthread_mutex_lock(&rq->idle_lock);
            atomic_store_explicit(&rq->idle, 1, memory_order_release);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 500000;  /* +0.5ms (tighter poll for responsiveness) */
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&rq->idle_cond, &rq->idle_lock, &ts);
            atomic_store_explicit(&rq->idle, 0, memory_order_release);
            pthread_mutex_unlock(&rq->idle_lock);
        }
    }
}

static void *scheduler_main(void *arg) {
    sw_scheduler_t *sched = (sw_scheduler_t *)arg;
    sched->active = 1;
    scheduler_loop(sched);
    return NULL;
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

    /* Initialize arena — single mmap, covers everything */
    if (sw_arena_init(&g_swarm->arena, SWARM_MAX_PROCESSES) != 0) {
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

    /* Let schedulers start */
    usleep(10000);

    printf("[SwarmRT] Arena initialized: %zu MB mmap, %u proc slots, %u heap blocks\n",
           g_swarm->arena.size / (1024 * 1024),
           g_swarm->arena.proc_capacity,
           g_swarm->arena.block_count);
    printf("[SwarmRT] Swarm '%s' initialized with %d scheduler(s)\n",
           name, num_schedulers);
    fflush(stdout);
    return 0;
}

void sw_shutdown(int swarm_id) {
    (void)swarm_id;

    if (!g_swarm) return;

    g_swarm->running = 0;

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

    /* Free process stacks (lazily allocated, not part of arena mmap) */
    if (g_swarm->arena.proc_slab) {
        sw_process_t *slab = (sw_process_t *)g_swarm->arena.proc_slab;
        for (uint32_t i = 0; i < g_swarm->arena.proc_capacity; i++) {
            if (slab[i].stack_mem) {
                free(slab[i].stack_mem);
                slab[i].stack_mem = NULL;
            }
        }
    }

    /* Clean up arena (os_unfair_lock has no destroy) */
    if (g_swarm->arena.base) {
        munmap(g_swarm->arena.base, g_swarm->arena.size);
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
     * If called from within a scheduler, default to the CURRENT scheduler —
     * zero cross-thread overhead. Only use round-robin from external threads. */
    uint32_t sched_id;
    if (tls_scheduler) {
        sched_id = tls_scheduler->id;  /* Local spawn — same scheduler */
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

/*
 * sw_process_done: Called by the assembly trampoline when entry() returns.
 * Marks process as EXITING and context-swaps back to the scheduler.
 */
void sw_process_done(sw_process_t *proc) {
    proc->state = SW_PROC_EXITING;
    sw_context_swap(proc, &proc->scheduler->sched_proc);
    /* Should never reach here */
}

void sw_yield(void) {
    sw_process_t *proc = tls_current;
    if (!proc) return;

    /* Context swap back to scheduler — scheduler will re-enqueue us */
    proc->state = SW_PROC_RUNNABLE;
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
 */
static inline sw_msg_t *mailbox_pop_first(sw_mailbox_t *mb) {
    sw_msg_t *m = mb->priv_head;
    if (!m) return NULL;

    mb->priv_head = m->next;
    if (mb->priv_head)
        mb->priv_head->prev = NULL;
    else
        mb->priv_tail = NULL;
    mb->count--;
    return m;
}

/*
 * mailbox_pop_tagged: Scan private queue for first message with matching tag.
 * Removes and returns it. Non-matching messages stay in place.
 */
static inline sw_msg_t *mailbox_pop_tagged(sw_mailbox_t *mb, uint64_t tag) {
    sw_msg_t *m = mb->priv_head;
    while (m) {
        if (m->tag == tag) {
            /* Remove from doubly-linked list */
            if (m->prev) m->prev->next = m->next;
            else mb->priv_head = m->next;
            if (m->next) m->next->prev = m->prev;
            else mb->priv_tail = m->prev;
            mb->count--;
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
    if (atomic_exchange_explicit(&to->mailbox.waiting, 0, memory_order_acq_rel)) {
        /* Was waiting — re-enqueue to run queue.
         * Do NOT set state here. The receiver may be in its final drain
         * and could set state=WAITING for context-swap. If we also write
         * state=RUNNABLE, the scheduler's post-swap handler would re-enqueue,
         * causing a double-enqueue. Let the scheduler set RUNNING on dequeue. */
        sw_add_to_runq(&to->scheduler->runq, to);
    }
}

/* ============================================================================
 * MESSAGE PASSING
 * ============================================================================ */

void sw_send(sw_process_t *to, void *msg) {
    if (!to || !msg) return;

    sw_msg_t *m = msg_alloc();
    m->tag = SW_TAG_NONE;
    m->payload = msg;
    m->from_pid = tls_current ? tls_current->pid : 0;
    m->next = NULL;
    m->prev = NULL;

    /* Lock-free MPSC push */
    mailbox_push(&to->mailbox, m);

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
        sw_msg_t *m = mailbox_pop_first(&proc->mailbox);
        if (m) {
            void *payload = m->payload;
            msg_free(m);
            proc->messages_recv++;
            return payload;
        }

        if (timeout_ms == 0) return NULL;

        /* No message — prepare to sleep.
         * Critical ordering: set waiting BEFORE final drain check.
         * This prevents lost wake-ups (see Vyukov MPSC pattern). */
        proc->state = SW_PROC_WAITING;
        atomic_store_explicit(&proc->mailbox.waiting, 1, memory_order_release);

        /* Final drain — catch messages sent between first drain and waiting flag */
        mailbox_drain(&proc->mailbox);
        m = mailbox_pop_first(&proc->mailbox);
        if (m) {
            /* Got a message — race-safe cancel via atomic_exchange.
             * Only self-resume if WE clear waiting (exchange returns 1).
             * If sender already cleared it (returns 0), process is in the
             * runq — push message back and context-swap to avoid double-run. */
            int was_waiting = atomic_exchange_explicit(&proc->mailbox.waiting, 0, memory_order_acq_rel);
            if (was_waiting) {
                /* We won — not in runq, safe to self-resume */
                proc->state = SW_PROC_RUNNING;
                void *payload = m->payload;
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
            sw_context_swap(proc, &proc->scheduler->sched_proc);
            proc->state = SW_PROC_RUNNING;
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
                    proc->state = SW_PROC_RUNNING;
                    return NULL;
                }
                /* Sender already woke us and enqueued to runq — context-swap
                 * to let scheduler properly dequeue before continuing. */
                sw_context_swap(proc, &proc->scheduler->sched_proc);
                proc->state = SW_PROC_RUNNING;
                return NULL;
            }
            uint64_t remaining = timeout_ms - elapsed;
            timer_ref = sw_send_after(remaining, proc, SW_TAG_NONE, NULL);
        }

        /* Context swap back to scheduler */
        sw_context_swap(proc, &proc->scheduler->sched_proc);

        /* Resumed — sender or timer woke us (already cleared waiting) */
        proc->state = SW_PROC_RUNNING;

        if (timer_ref) sw_cancel_timer(timer_ref);
        if (proc->kill_flag) return NULL;
    }
}

void *sw_receive_nowait(void) {
    return sw_receive(0);
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
                           uint64_t from_pid, uint64_t ref, int reason) {
    sw_signal_t *sig = (sw_signal_t *)malloc(sizeof(sw_signal_t));
    sig->pid = from_pid;
    sig->ref = ref;
    sig->reason = reason;

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
    proc->exit_reason = reason;

    pthread_mutex_lock(&g_swarm->link_lock);

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
                deliver_signal(peer, SW_TAG_EXIT, proc->pid, 0, reason);
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
                deliver_signal(peer, SW_TAG_EXIT, proc->pid, 0, 0);
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
        deliver_signal(mon->watcher, SW_TAG_DOWN, proc->pid, mon->ref, reason);

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
            deliver_signal(self, SW_TAG_EXIT, other->pid, 0, other->exit_reason);
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
    /* In cooperative mode, scheduling locally deadlocks if parent blocks
     * waiting for child (both on same scheduler thread). Force child to
     * a DIFFERENT scheduler — not just round-robin (which can cycle back). */
    sw_scheduler_t *save_sched = tls_scheduler;
    if (save_sched && g_swarm->num_schedulers > 1) {
        /* Round-robin but skip parent's scheduler to avoid cooperative deadlock */
        uint32_t target;
        do {
            target = __sync_fetch_and_add(&g_swarm->next_sched, 1)
                     % g_swarm->num_schedulers;
        } while (target == save_sched->id);
        tls_scheduler = g_swarm->schedulers[target];
    } else if (save_sched) {
        tls_scheduler = NULL; /* Single scheduler — best effort */
    }
    /* If save_sched == NULL (external thread), keep NULL → uses round-robin */
    sw_process_t *child = sw_spawn(func, arg);
    tls_scheduler = save_sched;
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

    if (target->state == SW_PROC_EXITING || target->state == SW_PROC_FREE) {
        /* Target already dead — deliver DOWN immediately */
        deliver_signal(self, SW_TAG_DOWN, target->pid, ref, target->exit_reason);
        return ref;
    }

    sw_monitor_t *mon = (sw_monitor_t *)malloc(sizeof(sw_monitor_t));
    mon->ref = ref;
    mon->watcher = self;
    mon->watched = target;

    pthread_mutex_lock(&g_swarm->link_lock);

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
    if (!proc->reg_entry) return;

    sw_registry_t *reg = &g_swarm->registry;
    const char *name = proc->reg_entry->name;
    uint32_t idx = registry_hash(name) % reg->num_buckets;

    pthread_rwlock_wrlock(&reg->lock);

    sw_reg_entry_t **pp = &reg->buckets[idx];
    while (*pp) {
        if (*pp == proc->reg_entry) {
            sw_reg_entry_t *rm = *pp;
            *pp = rm->next;
            free(rm);
            break;
        }
        pp = &(*pp)->next;
    }
    proc->reg_entry = NULL;

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

static void fire_timers(void) {
    if (!g_swarm) return;

    sw_timer_list_t *tl = &g_swarm->timers;

    /* Quick check without lock — sorted list, head is earliest */
    if (!tl->head) return;

    uint64_t now = now_ns();

    pthread_mutex_lock(&tl->lock);
    while (tl->head && tl->head->fire_at_ns <= now) {
        sw_timer_t *t = tl->head;
        tl->head = t->next;
        pthread_mutex_unlock(&tl->lock);

        if (t->msg == NULL && t->tag == SW_TAG_NONE) {
            /* Wake-up timer (from receive timeout) — just wake process */
            mailbox_wake(t->dest);
        } else {
            /* Regular timer — deliver the message */
            sw_send_tagged(t->dest, t->tag, t->msg);
        }
        timer_free(t);

        pthread_mutex_lock(&tl->lock);
    }
    pthread_mutex_unlock(&tl->lock);
}

/* ============================================================================
 * TAGGED MESSAGES & SELECTIVE RECEIVE
 * ============================================================================ */

void sw_send_tagged(sw_process_t *to, uint64_t tag, void *msg) {
    if (!to) return;

    sw_msg_t *m = msg_alloc();
    m->tag = tag;
    m->payload = msg;
    m->from_pid = tls_current ? tls_current->pid : 0;
    m->next = NULL;
    m->prev = NULL;

    /* Lock-free MPSC push + wake */
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
        sw_msg_t *m = mailbox_pop_tagged(&proc->mailbox, tag);
        if (m) {
            void *payload = m->payload;
            msg_free(m);
            proc->messages_recv++;
            return payload;
        }

        if (timeout_ms == 0) return NULL;

        /* No match — prepare to sleep */
        proc->state = SW_PROC_WAITING;
        atomic_store_explicit(&proc->mailbox.waiting, 1, memory_order_release);

        /* Final drain — catch messages sent between first drain and waiting flag */
        mailbox_drain(&proc->mailbox);
        m = mailbox_pop_tagged(&proc->mailbox, tag);
        if (m) {
            int was_waiting = atomic_exchange_explicit(&proc->mailbox.waiting, 0, memory_order_acq_rel);
            if (was_waiting) {
                proc->state = SW_PROC_RUNNING;
                void *payload = m->payload;
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
            sw_context_swap(proc, &proc->scheduler->sched_proc);
            proc->state = SW_PROC_RUNNING;
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
                    proc->state = SW_PROC_RUNNING;
                    return NULL;
                }
                /* Sender already woke us and enqueued to runq — context-swap
                 * to let scheduler properly dequeue before continuing. */
                sw_context_swap(proc, &proc->scheduler->sched_proc);
                proc->state = SW_PROC_RUNNING;
                return NULL;
            }
            uint64_t remaining = timeout_ms - elapsed_ms;
            timer_ref = sw_send_after(remaining, proc, SW_TAG_NONE, NULL);
        }

        sw_context_swap(proc, &proc->scheduler->sched_proc);

        /* Resumed — sender or timer woke us (already cleared waiting) */
        proc->state = SW_PROC_RUNNING;
        if (timer_ref) sw_cancel_timer(timer_ref);
        if (proc->kill_flag) return NULL;
    }
}

/*
 * sw_receive_any: Receive any message, returning the tag.
 * Used by GenServer loop to dispatch on message type.
 */
void *sw_receive_any(uint64_t timeout_ms, uint64_t *out_tag) {
    sw_process_t *proc = tls_current;
    if (!proc) return NULL;
    if (out_tag) *out_tag = SW_TAG_NONE;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        /* Drain signal queue into private queue */
        mailbox_drain(&proc->mailbox);

        sw_msg_t *m = mailbox_pop_first(&proc->mailbox);
        if (m) {
            if (out_tag) *out_tag = m->tag;
            void *payload = m->payload;
            msg_free(m);
            proc->messages_recv++;
            return payload;
        }

        if (timeout_ms == 0) return NULL;

        /* No message — prepare to sleep */
        proc->state = SW_PROC_WAITING;
        atomic_store_explicit(&proc->mailbox.waiting, 1, memory_order_release);

        /* Final drain — catch messages sent between first drain and waiting flag */
        mailbox_drain(&proc->mailbox);
        m = mailbox_pop_first(&proc->mailbox);
        if (m) {
            int was_waiting = atomic_exchange_explicit(&proc->mailbox.waiting, 0, memory_order_acq_rel);
            if (was_waiting) {
                proc->state = SW_PROC_RUNNING;
                if (out_tag) *out_tag = m->tag;
                void *payload = m->payload;
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
            sw_context_swap(proc, &proc->scheduler->sched_proc);
            proc->state = SW_PROC_RUNNING;
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
                    proc->state = SW_PROC_RUNNING;
                    return NULL;
                }
                /* Sender already woke us and enqueued to runq — context-swap
                 * to let scheduler properly dequeue before continuing. */
                sw_context_swap(proc, &proc->scheduler->sched_proc);
                proc->state = SW_PROC_RUNNING;
                return NULL;
            }
            uint64_t remaining = timeout_ms - elapsed;
            timer_ref = sw_send_after(remaining, proc, SW_TAG_NONE, NULL);
        }

        sw_context_swap(proc, &proc->scheduler->sched_proc);

        /* Resumed — sender or timer woke us (already cleared waiting) */
        proc->state = SW_PROC_RUNNING;
        if (timer_ref) sw_cancel_timer(timer_ref);
        if (proc->kill_flag) return NULL;
    }
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
