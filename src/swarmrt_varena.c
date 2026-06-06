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

sw_value_arena_t *sw_varena_create(size_t first_chunk) {
    sw_value_arena_t *a = (sw_value_arena_t *)malloc(sizeof(sw_value_arena_t));
    if (!a) return NULL;
    if (first_chunk < 1024) first_chunk = 8192;
    a->head = chunk_new(first_chunk);
    if (!a->head) { free(a); return NULL; }
    a->total_bytes = 0;
    a->chunk_count = 1;
    return a;
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
