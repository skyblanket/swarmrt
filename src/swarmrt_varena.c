/* swarmrt_varena.c — per-process value arena. See swarmrt_varena.h. */
#include "swarmrt_varena.h"
#include <stdlib.h>
#include <string.h>

#define SW_VARENA_ALIGN 16

static sw_varena_chunk_t *chunk_new(size_t cap) {
    sw_varena_chunk_t *c = (sw_varena_chunk_t *)malloc(sizeof(sw_varena_chunk_t) + cap);
    if (!c) return NULL;
    c->next = NULL;
    c->used = 0;
    c->cap = cap;
    return c;
}

static inline char *chunk_data(sw_varena_chunk_t *c) {
    return (char *)(c + 1);
}

static sw_value_arena_t *varena_new(size_t first_chunk, sw_region_kind_t kind) {
    sw_value_arena_t *a = (sw_value_arena_t *)malloc(sizeof(sw_value_arena_t));
    if (!a) return NULL;
    a->head = chunk_new(first_chunk);
    if (!a->head) { free(a); return NULL; }
    a->tail = a->head;   /* oldest chunk; never changes (new chunks prepend at head) */
    a->total_bytes = 0;
    a->chunk_count = 1;
    a->kind = kind;
    a->list_next = NULL;
    return a;
}

sw_value_arena_t *sw_varena_create(size_t first_chunk) {
    if (first_chunk < 1024) first_chunk = 8192;   /* process arena: 8KB floor */
    return varena_new(first_chunk, SW_REGION_PROCESS);
}

/* Right-sized region: clamp(hint, 256B, 64KB), NO 8KB floor — a tiny message
 * costs a 256B chunk, the 32KB spawn arg one ~right-sized chunk. */
sw_value_arena_t *sw_varena_create_kind(size_t hint, sw_region_kind_t kind) {
    if (hint < 256) hint = 256;
    else if (hint > 64 * 1024) hint = 64 * 1024;
    return varena_new(hint, kind);
}

/* O(1) splice: move src's chunk list onto dst, then free src's header. dst->head
 * becomes src's (full) head, so the next dst alloc trips used+n>cap and grows a
 * fresh chunk — correct, one partially-used chunk's tail goes unused. src is dead
 * after this (header freed; chunks now owned by dst, reclaimed on dst free). */
sw_varena_mark_t sw_varena_mark(sw_value_arena_t *a) {
    sw_varena_mark_t m = { NULL, 0, 0 };
    if (a) { m.chunk = a->head; m.used = a->head ? a->head->used : 0; m.total_bytes = a->total_bytes; }
    return m;
}

/* Scoped reset: free chunks newer than mark.chunk, rewind mark.chunk to
 * mark.used. Chunks below the mark (caller's values) are untouched. */
void sw_varena_reset_to(sw_value_arena_t *a, sw_varena_mark_t mark) {
    if (!a || !mark.chunk) return;
    sw_varena_chunk_t *c = a->head;
    while (c && c != mark.chunk) {
        sw_varena_chunk_t *next = c->next;
#ifdef SW_ARENA_POISON
        memset((char *)(c + 1), 0xDE, c->cap);
#endif
        free(c);
        a->chunk_count--;
        c = next;
    }
    /* c == mark.chunk (still in the list, below the freed ones). */
    a->head = mark.chunk;
#ifdef SW_ARENA_POISON
    if (mark.chunk->used > mark.used)
        memset((char *)(mark.chunk + 1) + mark.used, 0xDE, mark.chunk->used - mark.used);
#endif
    mark.chunk->used = mark.used;
    a->total_bytes = mark.total_bytes;
}

void sw_varena_adopt(sw_value_arena_t *dst, sw_value_arena_t *src) {
    if (!dst || !src) return;
    if (src->head && src->tail) {
        /* O(1): src->tail is src's oldest chunk (tracked, not walked). Link it
         * onto dst's stack and make src's newest chunk dst's alloc point. dst's
         * own tail (oldest) is unchanged. */
        src->tail->next = dst->head;
        dst->head = src->head;
        dst->total_bytes += src->total_bytes;
        dst->chunk_count += src->chunk_count;
    }
    free(src);                       /* header only; chunks transferred to dst */
}

void *sw_varena_alloc(sw_value_arena_t *a, size_t n) {
    if (!a) return NULL;
    n = (n + (SW_VARENA_ALIGN - 1)) & ~(size_t)(SW_VARENA_ALIGN - 1);
    sw_varena_chunk_t *c = a->head;
    if (c->used + n > c->cap) {
        /* Grow: new head chunk sized max(n, 2*current cap). The new chunk
         * becomes head; the old one stays linked behind it (still live). */
        size_t ncap = c->cap * 2;
        if (ncap < n) ncap = n;
        sw_varena_chunk_t *nc = chunk_new(ncap);
        if (!nc) return NULL;
        nc->next = c;
        a->head = nc;
        c = nc;
        a->chunk_count++;
    }
    void *p = chunk_data(c) + c->used;
    c->used += n;
    a->total_bytes += n;
    return p;
}

char *sw_varena_strdup(sw_value_arena_t *a, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)sw_varena_alloc(a, n);
    if (p) memcpy(p, s, n);
    return p;
}

void *sw_varena_memdup(sw_value_arena_t *a, const void *src, size_t n) {
    void *p = sw_varena_alloc(a, n ? n : 1);
    if (p && src && n) memcpy(p, src, n);
    return p;
}

void sw_varena_free_all(sw_value_arena_t *a) {
    if (!a) return;
    sw_varena_chunk_t *c = a->head;
    while (c) {
        sw_varena_chunk_t *next = c->next;
#ifdef SW_ARENA_POISON
        memset(chunk_data(c), 0xDE, c->cap);
#endif
        free(c);
        c = next;
    }
    free(a);
}
