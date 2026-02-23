/*
 * SwarmRT Phase 8: Garbage Collection & Heap Management
 *
 * Conservative GC for per-process heaps. Since we use void* pointers,
 * we can't distinguish pointers from integers — so we treat any value
 * that looks like a heap pointer as a live reference (conservative scan).
 *
 * Minor GC: Mark-sweep within the current heap block.
 * Major GC: Allocate a fresh heap block, copy marked data, free old block.
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "swarmrt_gc.h"

/* Per-process GC state stored in process flags */
static __thread sw_gc_stats_t tls_gc_stats;

/* === Time Helpers === */

static uint64_t gc_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* === Heap Queries === */

int sw_heap_contains(sw_process_t *proc, void *ptr) {
    if (!proc || !ptr) return 0;
    uint64_t *p = (uint64_t *)ptr;
    return (p >= proc->heap.start && p < proc->heap.end);
}

size_t sw_heap_used(sw_process_t *proc) {
    if (!proc) return 0;
    return (size_t)(proc->heap.top - proc->heap.start);
}

size_t sw_heap_size(sw_process_t *proc) {
    if (!proc) return 0;
    return proc->heap.size;
}

/* === Bitmap for mark phase === */

#define GC_BITMAP_WORDS(n) (((n) + 63) / 64)

typedef struct {
    uint64_t *bits;
    size_t num_words;
} gc_bitmap_t;

static gc_bitmap_t bitmap_alloc(size_t heap_words) {
    gc_bitmap_t bm;
    bm.num_words = GC_BITMAP_WORDS(heap_words);
    bm.bits = (uint64_t *)calloc(bm.num_words, sizeof(uint64_t));
    return bm;
}

static void bitmap_free(gc_bitmap_t *bm) {
    free(bm->bits);
    bm->bits = NULL;
}

static void bitmap_set(gc_bitmap_t *bm, size_t idx) {
    bm->bits[idx / 64] |= (1ULL << (idx % 64));
}

static int bitmap_get(gc_bitmap_t *bm, size_t idx) {
    return (bm->bits[idx / 64] >> (idx % 64)) & 1;
}

/* === Conservative Mark Phase ===
 *
 * Scan a memory range for values that look like heap pointers.
 * For each match, mark the corresponding word in the bitmap.
 * Conservative: may retain dead objects, but never misses live ones.
 */

static void gc_mark_range(gc_bitmap_t *bm, uint64_t *start, size_t count,
                          uint64_t *heap_start, uint64_t *heap_end) {
    for (size_t i = 0; i < count; i++) {
        uint64_t val = start[i];
        /* Check if this value looks like a pointer into the heap */
        uint64_t *p = (uint64_t *)val;
        if (p >= heap_start && p < heap_end) {
            size_t idx = (size_t)(p - heap_start);
            bitmap_set(bm, idx);
            /* Also mark adjacent words (conservative: object might span multiple words) */
            if (idx > 0) bitmap_set(bm, idx - 1);
            if (idx + 1 < (size_t)(heap_end - heap_start)) bitmap_set(bm, idx + 1);
        }
    }
}

/* === Minor GC: Mark-Compact within current heap ===
 *
 * 1. Allocate bitmap
 * 2. Mark roots (mailbox pointers)
 * 3. Compact: slide live words down to fill gaps
 * 4. Update top pointer
 */

int sw_gc_minor(sw_process_t *proc) {
    if (!proc) return -1;

    uint64_t t0 = gc_now_us();

    size_t heap_words = (size_t)(proc->heap.top - proc->heap.start);
    if (heap_words == 0) return 0; /* Nothing to collect */

    gc_bitmap_t bm = bitmap_alloc(heap_words);

    /* Mark roots: scan mailbox payloads (private queue only —
     * GC runs from within the process, so no concurrent drain needed) */
    sw_msg_t *msg = proc->mailbox.priv_head;
    while (msg) {
        if (msg->payload) {
            uint64_t val = (uint64_t)msg->payload;
            uint64_t *p = (uint64_t *)val;
            if (p >= proc->heap.start && p < proc->heap.end) {
                size_t idx = (size_t)(p - proc->heap.start);
                bitmap_set(&bm, idx);
            }
        }
        msg = msg->next;
    }

    /* Compact: count unmarked words */
    size_t live_words = 0;
    for (size_t i = 0; i < heap_words; i++) {
        if (bitmap_get(&bm, i)) live_words++;
    }

    size_t reclaimed = heap_words - live_words;

    /* Simple compaction: if >25% is garbage, copy live words to start */
    if (reclaimed > heap_words / 4) {
        size_t dst = 0;
        for (size_t src = 0; src < heap_words; src++) {
            if (bitmap_get(&bm, src)) {
                if (dst != src) {
                    proc->heap.start[dst] = proc->heap.start[src];
                }
                dst++;
            }
        }
        proc->heap.top = proc->heap.start + dst;
    }

    bitmap_free(&bm);

    uint64_t elapsed = gc_now_us() - t0;

    /* Update stats */
    tls_gc_stats.minor_gcs++;
    tls_gc_stats.words_reclaimed += reclaimed;
    tls_gc_stats.total_gc_time_us += elapsed;

    return (int)reclaimed;
}

/* === Major GC: Full heap compaction ===
 *
 * Allocates a new heap block, copies only live data, frees old block.
 * This is the nuclear option — slower but reclaims maximum space.
 */

int sw_gc_major(sw_process_t *proc) {
    if (!proc) return -1;

    uint64_t t0 = gc_now_us();

    size_t heap_words = (size_t)(proc->heap.top - proc->heap.start);
    if (heap_words == 0) return 0;

    gc_bitmap_t bm = bitmap_alloc(heap_words);

    /* Mark roots: scan private mailbox queue */
    sw_msg_t *msg = proc->mailbox.priv_head;
    while (msg) {
        if (msg->payload) {
            uint64_t val = (uint64_t)msg->payload;
            uint64_t *p = (uint64_t *)val;
            if (p >= proc->heap.start && p < proc->heap.end) {
                size_t idx = (size_t)(p - proc->heap.start);
                bitmap_set(&bm, idx);
            }
        }
        msg = msg->next;
    }

    /* Count live words */
    size_t live_words = 0;
    for (size_t i = 0; i < heap_words; i++) {
        if (bitmap_get(&bm, i)) live_words++;
    }

    /* Allocate fresh heap (same size as current) */
    size_t new_size = proc->heap.size;
    uint64_t *new_heap = (uint64_t *)calloc(new_size, sizeof(uint64_t));
    if (!new_heap) {
        bitmap_free(&bm);
        return -1;
    }

    /* Copy live data */
    size_t dst = 0;
    for (size_t src = 0; src < heap_words; src++) {
        if (bitmap_get(&bm, src)) {
            new_heap[dst++] = proc->heap.start[src];
        }
    }

    /* Save old heap as "old generation" — only free if it was malloc'd */
    if (proc->heap.old_heap && !proc->heap.arena_backed)
        free(proc->heap.old_heap);
    proc->heap.old_heap = proc->heap.start;
    proc->heap.old_top = proc->heap.top;
    proc->heap.old_size = proc->heap.size;

    /* Install new heap (malloc'd — safe to free later) */
    proc->heap.start = new_heap;
    proc->heap.top = new_heap + dst;
    proc->heap.end = new_heap + new_size;
    proc->heap.arena_backed = 0;  /* new heap is malloc'd */
    proc->heap.gen_gcs++;

    size_t reclaimed = heap_words - live_words;
    bitmap_free(&bm);

    uint64_t elapsed = gc_now_us() - t0;

    tls_gc_stats.major_gcs++;
    tls_gc_stats.words_reclaimed += reclaimed;
    tls_gc_stats.total_gc_time_us += elapsed;

    return (int)reclaimed;
}

/* === Heap Allocation with GC === */

void *sw_heap_alloc(sw_process_t *proc, size_t words) {
    if (!proc) return NULL;

    /* Check if there's space */
    if (proc->heap.top + words > proc->heap.end) {
        /* Try minor GC first */
        sw_gc_minor(proc);

        if (proc->heap.top + words > proc->heap.end) {
            /* Major GC */
            sw_gc_major(proc);

            if (proc->heap.top + words > proc->heap.end) {
                /* Still no space — heap is genuinely full */
                return NULL;
            }
        }
    }

    uint64_t *ptr = proc->heap.top;
    proc->heap.top += words;
    return ptr;
}

sw_gc_stats_t sw_gc_stats(sw_process_t *proc) {
    (void)proc;
    return tls_gc_stats;
}
