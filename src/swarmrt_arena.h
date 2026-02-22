/*
 * SwarmRT Arena Allocator - Zero-syscall process spawning
 *
 * Architecture:
 *
 * 1. Single mmap at init — one syscall covers everything
 * 2. Per-scheduler partitions — each scheduler has its own free list
 *    with its own spinlock. No cross-scheduler contention on hot path.
 * 3. Steal on empty — if a partition runs out, steal a batch from another
 *
 * No shared allocator lock → scales to 128+ schedulers.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_ARENA_H
#define SWARMRT_ARENA_H

#include <stdint.h>
#include <stdatomic.h>

#ifdef __APPLE__
#include <os/lock.h>
typedef os_unfair_lock sw_spinlock_t;
#define SW_SPINLOCK_INIT OS_UNFAIR_LOCK_INIT
#define sw_spin_lock(l) os_unfair_lock_lock(l)
#define sw_spin_unlock(l) os_unfair_lock_unlock(l)
#else
#include <pthread.h>
typedef pthread_mutex_t sw_spinlock_t;
#define SW_SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define sw_spin_lock(l) pthread_mutex_lock(l)
#define sw_spin_unlock(l) pthread_mutex_unlock(l)
#endif

#define SW_MAX_PARTITIONS 64
#define SW_STEAL_BATCH    64   /* Steal this many slots at once to amortize */

/* Per-scheduler partition — own spinlock, own free lists.
 * Cache-line aligned to prevent false sharing. */
typedef struct {
    uint32_t *free_pids;        /* Stack of free slot indices */
    uint32_t pid_top;           /* Top of free PID stack */
    uint32_t pid_capacity;      /* Max slots in this partition */
    uint32_t *free_blocks;      /* Stack of free block indices */
    uint32_t block_top;         /* Top of free block stack */
    uint32_t block_capacity;    /* Max blocks in this partition */
    sw_spinlock_t lock;
    char _pad[64 - 28 - sizeof(sw_spinlock_t)]; /* Pad to ~cache line */
} __attribute__((aligned(64))) sw_arena_partition_t;

typedef struct {
    uint8_t *base;              /* Arena base address (mmap'd) */
    size_t size;                /* Total arena size */

    /* Process slab — pre-allocated array indexed by slot */
    void *proc_slab;            /* Cast to sw_process_t* after include */
    uint32_t proc_capacity;     /* Max process slots */

    /* Block pool — 2KB blocks for per-process heaps */
    uint8_t *block_base;        /* Start of block region */
    uint32_t block_size;        /* Bytes per block (2048) */
    uint32_t block_count;       /* Total blocks available */

    /* Per-scheduler partitions */
    sw_arena_partition_t partitions[SW_MAX_PARTITIONS];
    uint32_t num_partitions;

    /* Overflow partition for free list stacks (part of mmap) */
    uint32_t *pid_stack_base;   /* Base of all partition PID stacks */
    uint32_t *block_stack_base; /* Base of all partition block stacks */

    /* Monotonic PID counter (never reused, always increases) */
    _Atomic uint64_t next_pid;
} sw_arena_t;

/*
 * Pop from a partition (caller holds partition lock).
 */
static inline int32_t part_pop_pid(sw_arena_partition_t *p) {
    if (p->pid_top == 0) return -1;
    p->pid_top--;
    return (int32_t)p->free_pids[p->pid_top];
}

static inline int32_t part_pop_block(sw_arena_partition_t *p) {
    if (p->block_top == 0) return -1;
    p->block_top--;
    return (int32_t)p->free_blocks[p->block_top];
}

/*
 * Push to a partition (caller holds partition lock).
 */
static inline void part_push_pid(sw_arena_partition_t *p, uint32_t val) {
    p->free_pids[p->pid_top] = val;
    p->pid_top++;
}

static inline void part_push_block(sw_arena_partition_t *p, uint32_t val) {
    p->free_blocks[p->block_top] = val;
    p->block_top++;
}

/*
 * Steal a batch of PIDs from victim into dst.
 * Caller holds BOTH locks (dst first, then victim — consistent ordering).
 * Returns number stolen.
 */
static inline uint32_t part_steal_pids(sw_arena_partition_t *dst,
                                        sw_arena_partition_t *victim,
                                        uint32_t batch) {
    uint32_t avail = victim->pid_top;
    if (avail == 0) return 0;
    uint32_t n = (avail + 1) / 2;  /* Take ceil(half), leave floor(half) */
    if (n > batch) n = batch;
    for (uint32_t i = 0; i < n; i++) {
        victim->pid_top--;
        dst->free_pids[dst->pid_top] = victim->free_pids[victim->pid_top];
        dst->pid_top++;
    }
    return n;
}

static inline uint32_t part_steal_blocks(sw_arena_partition_t *dst,
                                          sw_arena_partition_t *victim,
                                          uint32_t batch) {
    uint32_t avail = victim->block_top;
    if (avail == 0) return 0;
    uint32_t n = (avail + 1) / 2;  /* Take ceil(half), leave floor(half) */
    if (n > batch) n = batch;
    for (uint32_t i = 0; i < n; i++) {
        victim->block_top--;
        dst->free_blocks[dst->block_top] = victim->free_blocks[victim->block_top];
        dst->block_top++;
    }
    return n;
}

#endif /* SWARMRT_ARENA_H */
