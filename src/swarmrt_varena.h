/* swarmrt_varena.h — per-process VALUE arena (GC v1).
 *
 * Distinct from swarmrt_arena.h (that one is the mmap'd process-slab / block
 * pool for zero-syscall spawning). This is a bump-allocated region attached
 * to each runtime process holding the sw_val_t nodes and owned blobs
 * (strings, tuple/list spines, map keys/vals, bytes) that the process builds
 * while running. On process exit (process_destroy) the whole varena is freed
 * in O(chunks) — no tracing, no refcounting, no relocation.
 *
 * SINGLE-WRITER: only the owning fiber allocates into it (a fiber never runs
 * concurrently with itself). Values that escape a process (sent messages,
 * spawn captures, ETS entries) are deep-copied to the GLOBAL heap at the
 * escape boundary, so no other thread touches this varena. Hence no lock.
 *
 * See [[swarmrt-gc-design]] in memory for the full design + escape audit.
 */
#ifndef SWARMRT_VARENA_H
#define SWARMRT_VARENA_H

#include <stddef.h>

typedef struct sw_varena_chunk {
    struct sw_varena_chunk *next;  /* previous chunk (singly-linked stack) */
    size_t used;
    size_t cap;
    /* `cap` bytes of storage follow this header inline. */
} sw_varena_chunk_t;

/* Ownership v2: a region is just a value arena tagged by who owns its lifecycle.
 * The arena IS the region (no separate wrapper — see [[swarmrt-gc-design]]). */
typedef enum {
    SW_REGION_PROCESS = 0,   /* a process's own values; freed at process exit / turn checkpoint */
    SW_REGION_MESSAGE,       /* a queued message payload; adopted by the receiver on match */
    SW_REGION_SPAWN,         /* spawn args/captures; adopted by the child on entry */
    SW_REGION_ETS,           /* a stored ETS value; freed on replace/delete/table-destroy */
    SW_REGION_PERSISTENT     /* explicit long-lived store */
} sw_region_kind_t;

typedef struct sw_value_arena {
    sw_varena_chunk_t *head;       /* current (most-recent) chunk; alloc point */
    sw_varena_chunk_t *tail;       /* oldest chunk (first created); for O(1) adopt */
    size_t total_bytes;            /* stat: bytes handed out */
    size_t chunk_count;            /* stat: number of chunks */
    sw_region_kind_t kind;         /* lifecycle owner class (v2) */
    struct sw_value_arena *list_next; /* per-process region chain (accounting/teardown) */
} sw_value_arena_t;

/* Create a SW_REGION_PROCESS arena with an initial chunk of at least
 * `first_chunk` bytes (floored to 8KB). Returns NULL on OOM. */
sw_value_arena_t *sw_varena_create(size_t first_chunk);

/* Create a region of `kind` whose first chunk is right-sized: clamp(hint,
 * 256B, 64KB) with NO 8KB floor — so a tiny message (an int, a 3-tuple) costs
 * a 256B chunk, not 8KB. Used for message + spawn regions. */
sw_value_arena_t *sw_varena_create_kind(size_t hint, sw_region_kind_t kind);

/* O(1) ownership transfer: splice all of `src`'s chunks onto `dst` (via src's
 * tracked tail — no walk) and free the `src` header (its chunks are now owned by
 * `dst`, reclaimed when `dst` is freed). After this `src` is dead. Used to adopt
 * a message region into the receiver's arena on match, and a spawn region into
 * the child's arena on entry. */
void sw_varena_adopt(sw_value_arena_t *dst, sw_value_arena_t *src);

/* Bump-allocate `n` bytes, 16-byte aligned. NOT zeroed (callers that need
 * zeroing — e.g. value nodes — memset themselves). Returns NULL on OOM. */
void *sw_varena_alloc(sw_value_arena_t *a, size_t n);

/* strdup / memdup into the arena. */
char *sw_varena_strdup(sw_value_arena_t *a, const char *s);
void *sw_varena_memdup(sw_value_arena_t *a, const void *src, size_t n);

/* Free every chunk and the arena header. After this the pointer is dead.
 * Build with -DSW_ARENA_POISON to fill freed chunks with 0xDE so any
 * escaped pointer that slipped past the copy-on-escape audit faults loudly. */
void sw_varena_free_all(sw_value_arena_t *a);

/* The current process's value arena, or NULL when there is none (the
 * interpreter, the REPL, or builtins running before any fiber). The value
 * constructors route allocations here. Strong impl in swarmrt_native.c reads
 * tls_current->varena; a weak stub in swarmrt_lang.c covers runtime-less links. */
sw_value_arena_t *sw_self_varena(void);

/* Per-process memory quota (SW_PROC_MEM_MAX env, bytes; 0/unset = unlimited).
 * Called from the arena GROW path (before chunk_new) and from adopt-splice —
 * NOT per-value, so the cost is one call on the cold path only. Enforcement
 * fires only when `a` is the CURRENT process's INSTALLED arena
 * (tls_current->varena == a): that covers every byte the process retains —
 * its own allocations plus adopted message/spawn/turn regions (adopt merges
 * total_bytes into the installed arena, where the next grow/adopt re-checks) —
 * while excluding the mid-copy hazard (a turn-checkpoint temp region grows
 * under g_alloc_target; panicking there would leak the alloc-target
 * thread-local into the next fiber on this scheduler thread). Over quota →
 * the strong impl (swarmrt_native.c) panics THE PROCESS (loud stderr banner
 * naming the quota + pid; sw_process_panic — links/monitors/supervisors see
 * a normal abnormal exit; the node and other processes survive). May not
 * return. Weak no-op stub below covers runtime-less links; the quota is
 * ARENA-SCOPED by design — the global-heap fallback (SW_GC_OFF, interpreter
 * values outside a fiber) is not metered. */
void sw_varena_quota_check(sw_value_arena_t *a, size_t need);

/* Ownership v2 turn-checkpoint: swap the current process's value arena (after
 * copying carry-forward state into the new one). No-op outside a process. */
void sw_set_self_varena(sw_value_arena_t *a);

/* A high-water mark into an arena — the (chunk, offset, bytes) at the moment a
 * tail-recursive function was entered. The "floor": everything below it belongs
 * to the CALLER and must be preserved; everything the function allocates is
 * above it. A SCOPED reset reclaims only above the floor (so a tail-recursive
 * helper called mid-computation can't free its caller's live values). */
typedef struct {
    sw_varena_chunk_t *chunk;
    size_t used;
    size_t total_bytes;
} sw_varena_mark_t;

sw_varena_mark_t sw_varena_mark(sw_value_arena_t *a);

/* Free every chunk newer than `mark.chunk` and rewind `mark.chunk` to
 * `mark.used` — reclaiming everything allocated since the mark while preserving
 * everything below it. The arena's alloc point becomes `mark.chunk` again. */
void sw_varena_reset_to(sw_value_arena_t *a, sw_varena_mark_t mark);

/* Turn-checkpoint threshold: a self-tail-call resets the process arena only once
 * it has handed out more than this many bytes since the last reset (total_bytes
 * only grows until a reset). This bounds a long-lived loop's memory to ~this
 * while avoiding a create+free per iteration for tight loops that allocate
 * little. Tunable; the gate just needs it well under the slope budget. */
#ifndef SW_TURN_RESET_BYTES
#define SW_TURN_RESET_BYTES (256 * 1024)
#endif

#endif /* SWARMRT_VARENA_H */
