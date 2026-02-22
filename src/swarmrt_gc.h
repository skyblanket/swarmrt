/*
 * SwarmRT Phase 8: Garbage Collection & Heap Management
 *
 * Per-process GC with two strategies:
 * 1. Minor GC: Copy live data from young generation to old generation
 * 2. Major GC: Compact the entire heap (old + young)
 *
 * Heap layout:
 * - Each process has a heap block (2KB from arena)
 * - When heap fills, GC runs before the next allocation
 * - Roots: process stack, mailbox, registered references
 *
 * Since SwarmRT uses void* pointers (not tagged values),
 * GC is conservative: it scans for pointers that fall within heap bounds.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_GC_H
#define SWARMRT_GC_H

#include "swarmrt_native.h"

/* GC statistics per process */
typedef struct {
    uint64_t minor_gcs;
    uint64_t major_gcs;
    uint64_t words_reclaimed;
    uint64_t total_gc_time_us;
} sw_gc_stats_t;

/* Allocate words on the process heap. Triggers GC if needed. */
void *sw_heap_alloc(sw_process_t *proc, size_t words);

/* Force a GC cycle on the current process */
int sw_gc_minor(sw_process_t *proc);
int sw_gc_major(sw_process_t *proc);

/* Get GC statistics for a process */
sw_gc_stats_t sw_gc_stats(sw_process_t *proc);

/* Check if a pointer is within a process's heap */
int sw_heap_contains(sw_process_t *proc, void *ptr);

/* Get heap usage stats */
size_t sw_heap_used(sw_process_t *proc);
size_t sw_heap_size(sw_process_t *proc);

#endif /* SWARMRT_GC_H */
