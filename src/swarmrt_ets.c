/*
 * SwarmRT ETS - Concurrent In-Memory Tables
 *
 * - Tables allocated via malloc (not arena — arena is for processes only)
 * - Hash table with 64 chained buckets per table
 * - Per-table pthread_rwlock_t — concurrent reads, exclusive writes
 * - Global pthread_mutex_t for table create/drop (rare operations)
 * - Owner process tracking — tables auto-deleted when owner exits
 * - Keys compared by pointer identity (phase 3)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include "swarmrt_ets.h"

/* === Internal Structures === */

#define SW_ETS_BUCKETS 64

typedef struct sw_ets_entry {
    void *key;
    void *value;
    struct sw_ets_entry *next;
} sw_ets_entry_t;

typedef struct sw_ets_table {
    sw_ets_tid_t tid;
    sw_ets_opts_t opts;
    sw_process_t *owner;
    pthread_rwlock_t rwlock;
    sw_ets_entry_t *buckets[SW_ETS_BUCKETS];
    uint32_t count;
    struct sw_ets_table *owner_next;  /* Owner's table list */
    struct sw_ets_table *global_next; /* Global table list */
} sw_ets_table_t;

/* === Global State === */

static pthread_mutex_t g_ets_meta_lock = PTHREAD_MUTEX_INITIALIZER;
static sw_ets_table_t *g_ets_tables = NULL;
static _Atomic uint32_t g_next_ets_tid = 1;

/* === Key Comparison (value equality for sw_val_t, pointer for raw) === */

#include "swarmrt_lang.h"

static int ets_key_eq(void *a, void *b) {
    if (a == b) return 1;
    /* Try sw_val_t value equality */
    sw_val_t *va = (sw_val_t *)a, *vb = (sw_val_t *)b;
    if (!va || !vb) return 0;
    if (va->type != vb->type) return 0;
    switch (va->type) {
        case SW_VAL_INT:    return va->v.i == vb->v.i;
        case SW_VAL_FLOAT:  return va->v.f == vb->v.f;
        case SW_VAL_ATOM:
        case SW_VAL_STRING: return va->v.str && vb->v.str && strcmp(va->v.str, vb->v.str) == 0;
        default: return a == b;
    }
}

/* Key ordering for ordered_set (returns <0, 0, >0) */
static int ets_key_cmp(void *a, void *b) {
    if (a == b) return 0;
    sw_val_t *va = (sw_val_t *)a, *vb = (sw_val_t *)b;
    if (!va || !vb) return (va ? 1 : -1);
    /* Type ordering: int < float < atom < string < other */
    if (va->type != vb->type) return (int)va->type - (int)vb->type;
    switch (va->type) {
        case SW_VAL_INT:    return (va->v.i < vb->v.i) ? -1 : (va->v.i > vb->v.i ? 1 : 0);
        case SW_VAL_FLOAT:  return (va->v.f < vb->v.f) ? -1 : (va->v.f > vb->v.f ? 1 : 0);
        case SW_VAL_ATOM:
        case SW_VAL_STRING: return strcmp(va->v.str ? va->v.str : "", vb->v.str ? vb->v.str : "");
        default: return (a < b) ? -1 : (a > b ? 1 : 0);
    }
}

/* === Hash Function === */

static uint32_t ets_hash_key(void *key) {
    /* Try value-based hash for sw_val_t keys */
    sw_val_t *v = (sw_val_t *)key;
    uint64_t k;
    if (v && v->type == SW_VAL_INT) k = (uint64_t)v->v.i;
    else if (v && v->type == SW_VAL_ATOM && v->v.str) {
        /* FNV-1a on string */
        k = 0xcbf29ce484222325ULL;
        for (const char *p = v->v.str; *p; p++) {
            k ^= (uint8_t)*p;
            k *= 0x100000001b3ULL;
        }
    } else if (v && v->type == SW_VAL_STRING && v->v.str) {
        k = 0xcbf29ce484222325ULL;
        for (const char *p = v->v.str; *p; p++) {
            k ^= (uint8_t)*p;
            k *= 0x100000001b3ULL;
        }
    } else {
        k = (uint64_t)(uintptr_t)key;
    }
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    return (uint32_t)(k % SW_ETS_BUCKETS);
}

/* === Table Lookup (caller must hold meta lock) === */

static sw_ets_table_t *ets_find_table_locked(sw_ets_tid_t tid) {
    sw_ets_table_t *t = g_ets_tables;
    while (t) {
        if (t->tid == tid) return t;
        t = t->global_next;
    }
    return NULL;
}

/* Find table with meta lock acquire/release */
static sw_ets_table_t *ets_find_table(sw_ets_tid_t tid) {
    pthread_mutex_lock(&g_ets_meta_lock);
    sw_ets_table_t *t = ets_find_table_locked(tid);
    pthread_mutex_unlock(&g_ets_meta_lock);
    return t;
}

/* === Access Control === */

static int ets_check_read(sw_ets_table_t *table) {
    if (table->opts.access == SW_ETS_PUBLIC) return 1;
    if (table->opts.access == SW_ETS_PROTECTED) return 1;
    /* Private: only owner can read */
    return (sw_self() == table->owner);
}

static int ets_check_write(sw_ets_table_t *table) {
    if (table->opts.access == SW_ETS_PUBLIC) return 1;
    /* Protected + Private: only owner can write */
    return (sw_self() == table->owner);
}

/* === Public API === */

sw_ets_tid_t sw_ets_new(sw_ets_opts_t opts) {
    sw_process_t *self = sw_self();

    sw_ets_table_t *table = (sw_ets_table_t *)calloc(1, sizeof(sw_ets_table_t));
    if (!table) return SW_ETS_INVALID_TID;

    table->tid = atomic_fetch_add(&g_next_ets_tid, 1);
    table->opts = opts;
    table->owner = self;
    table->count = 0;
    pthread_rwlock_init(&table->rwlock, NULL);

    pthread_mutex_lock(&g_ets_meta_lock);

    /* Link into global list */
    table->global_next = g_ets_tables;
    g_ets_tables = table;

    /* Link into owner's table list */
    if (self) {
        table->owner_next = (sw_ets_table_t *)self->ets_tables;
        self->ets_tables = table;
    }

    pthread_mutex_unlock(&g_ets_meta_lock);

    return table->tid;
}

int sw_ets_insert(sw_ets_tid_t tid, void *key, void *value) {
    sw_ets_table_t *table = ets_find_table(tid);
    if (!table) return -1;
    if (!ets_check_write(table)) return -1;

    uint32_t bucket = ets_hash_key(key);

    pthread_rwlock_wrlock(&table->rwlock);

    if (table->opts.type == SW_ETS_BAG) {
        /* Bag: always insert, allow duplicate keys */
        sw_ets_entry_t *entry = (sw_ets_entry_t *)malloc(sizeof(sw_ets_entry_t));
        if (!entry) { pthread_rwlock_unlock(&table->rwlock); return -1; }
        entry->key = key;
        entry->value = value;
        entry->next = table->buckets[bucket];
        table->buckets[bucket] = entry;
        table->count++;
    } else if (table->opts.type == SW_ETS_ORDERED_SET) {
        /* Ordered set: replace if key exists, else insert in sorted order */
        sw_ets_entry_t **pp = &table->buckets[bucket];
        while (*pp) {
            if (ets_key_eq((*pp)->key, key)) {
                (*pp)->value = value;
                pthread_rwlock_unlock(&table->rwlock);
                return 0;
            }
            if (ets_key_cmp(key, (*pp)->key) < 0) break;
            pp = &(*pp)->next;
        }
        sw_ets_entry_t *entry = (sw_ets_entry_t *)malloc(sizeof(sw_ets_entry_t));
        if (!entry) { pthread_rwlock_unlock(&table->rwlock); return -1; }
        entry->key = key;
        entry->value = value;
        entry->next = *pp;
        *pp = entry;
        table->count++;
    } else {
        /* Set: replace if key exists (value equality) */
        sw_ets_entry_t *e = table->buckets[bucket];
        while (e) {
            if (ets_key_eq(e->key, key)) {
                e->value = value;
                pthread_rwlock_unlock(&table->rwlock);
                return 0;
            }
            e = e->next;
        }
        sw_ets_entry_t *entry = (sw_ets_entry_t *)malloc(sizeof(sw_ets_entry_t));
        if (!entry) { pthread_rwlock_unlock(&table->rwlock); return -1; }
        entry->key = key;
        entry->value = value;
        entry->next = table->buckets[bucket];
        table->buckets[bucket] = entry;
        table->count++;
    }

    pthread_rwlock_unlock(&table->rwlock);
    return 0;
}

void *sw_ets_lookup(sw_ets_tid_t tid, void *key) {
    sw_ets_table_t *table = ets_find_table(tid);
    if (!table) return NULL;
    if (!ets_check_read(table)) return NULL;

    uint32_t bucket = ets_hash_key(key);

    pthread_rwlock_rdlock(&table->rwlock);

    sw_ets_entry_t *e = table->buckets[bucket];
    while (e) {
        if (ets_key_eq(e->key, key)) {
            void *val = e->value;
            pthread_rwlock_unlock(&table->rwlock);
            return val;
        }
        e = e->next;
    }

    pthread_rwlock_unlock(&table->rwlock);
    return NULL;
}

int64_t sw_ets_update_counter(sw_ets_tid_t tid, void *key, int64_t delta, int64_t initial) {
    sw_ets_table_t *table = ets_find_table(tid);
    if (!table) return initial;
    if (!ets_check_write(table)) return initial;

    uint32_t bucket = ets_hash_key(key);
    pthread_rwlock_wrlock(&table->rwlock);

    sw_ets_entry_t *e = table->buckets[bucket];
    while (e) {
        if (ets_key_eq(e->key, key)) {
            sw_val_t *val = (sw_val_t *)e->value;
            if (val && val->type == SW_VAL_INT) {
                val->v.i += delta;
                int64_t result = val->v.i;
                pthread_rwlock_unlock(&table->rwlock);
                return result;
            }
            pthread_rwlock_unlock(&table->rwlock);
            return initial;
        }
        e = e->next;
    }

    /* Not found — insert initial value */
    sw_val_t *new_val = sw_val_int(initial);
    sw_ets_entry_t *entry = (sw_ets_entry_t *)malloc(sizeof(sw_ets_entry_t));
    if (!entry) { pthread_rwlock_unlock(&table->rwlock); return initial; }
    entry->key = key;
    entry->value = new_val;
    entry->next = table->buckets[bucket];
    table->buckets[bucket] = entry;
    table->count++;
    pthread_rwlock_unlock(&table->rwlock);
    return initial;
}

int sw_ets_cas(sw_ets_tid_t tid, void *key, void *expected, void *new_value) {
    sw_ets_table_t *table = ets_find_table(tid);
    if (!table) return 0;
    if (!ets_check_write(table)) return 0;

    uint32_t bucket = ets_hash_key(key);
    pthread_rwlock_wrlock(&table->rwlock);

    sw_ets_entry_t *e = table->buckets[bucket];
    while (e) {
        if (ets_key_eq(e->key, key)) {
            sw_val_t *cur = (sw_val_t *)e->value;
            sw_val_t *exp = (sw_val_t *)expected;
            if (cur && exp && cur->type == exp->type) {
                int match = 0;
                switch (cur->type) {
                    case SW_VAL_INT:    match = (cur->v.i == exp->v.i); break;
                    case SW_VAL_FLOAT:  match = (cur->v.f == exp->v.f); break;
                    case SW_VAL_ATOM:
                    case SW_VAL_STRING: match = cur->v.str && exp->v.str && strcmp(cur->v.str, exp->v.str) == 0; break;
                    default:            match = (cur == exp); break;
                }
                if (match) {
                    e->value = new_value;
                    pthread_rwlock_unlock(&table->rwlock);
                    return 1;
                }
            } else if (!cur && !exp) {
                e->value = new_value;
                pthread_rwlock_unlock(&table->rwlock);
                return 1;
            }
            pthread_rwlock_unlock(&table->rwlock);
            return 0;
        }
        e = e->next;
    }

    pthread_rwlock_unlock(&table->rwlock);
    return 0;
}

void *sw_ets_take(sw_ets_tid_t tid, void *key) {
    sw_ets_table_t *table = ets_find_table(tid);
    if (!table) return NULL;
    if (!ets_check_write(table)) return NULL;

    uint32_t bucket = ets_hash_key(key);
    pthread_rwlock_wrlock(&table->rwlock);

    sw_ets_entry_t **pp = &table->buckets[bucket];
    while (*pp) {
        if (ets_key_eq((*pp)->key, key)) {
            sw_ets_entry_t *rm = *pp;
            void *val = rm->value;
            *pp = rm->next;
            free(rm);
            table->count--;
            pthread_rwlock_unlock(&table->rwlock);
            return val;
        }
        pp = &(*pp)->next;
    }

    pthread_rwlock_unlock(&table->rwlock);
    return NULL;
}

void *sw_ets_update(sw_ets_tid_t tid, void *key, void *fun) {
    (void)tid; (void)key; (void)fun;
    /* Deferred: function-valued builtins require closure capture in C */
    return NULL;
}

int sw_ets_delete(sw_ets_tid_t tid, void *key) {
    sw_ets_table_t *table = ets_find_table(tid);
    if (!table) return -1;
    if (!ets_check_write(table)) return -1;

    uint32_t bucket = ets_hash_key(key);

    pthread_rwlock_wrlock(&table->rwlock);

    sw_ets_entry_t **pp = &table->buckets[bucket];
    while (*pp) {
        if (ets_key_eq((*pp)->key, key)) {
            sw_ets_entry_t *rm = *pp;
            *pp = rm->next;
            free(rm);
            table->count--;
            /* For bag: remove ALL entries with this key */
            if (table->opts.type == SW_ETS_BAG) {
                while (*pp) {
                    if (ets_key_eq((*pp)->key, key)) {
                        rm = *pp;
                        *pp = rm->next;
                        free(rm);
                        table->count--;
                    } else {
                        pp = &(*pp)->next;
                    }
                }
            }
            pthread_rwlock_unlock(&table->rwlock);
            return 0;
        }
        pp = &(*pp)->next;
    }

    pthread_rwlock_unlock(&table->rwlock);
    return -1; /* Key not found */
}

int sw_ets_drop(sw_ets_tid_t tid) {
    pthread_mutex_lock(&g_ets_meta_lock);

    /* Find and unlink from global list */
    sw_ets_table_t *table = NULL;
    sw_ets_table_t **pp = &g_ets_tables;
    while (*pp) {
        if ((*pp)->tid == tid) {
            table = *pp;
            *pp = table->global_next;
            break;
        }
        pp = &(*pp)->global_next;
    }

    if (!table) {
        pthread_mutex_unlock(&g_ets_meta_lock);
        return -1;
    }

    /* Unlink from owner's table list */
    if (table->owner) {
        sw_ets_table_t **op = (sw_ets_table_t **)&table->owner->ets_tables;
        while (*op) {
            if (*op == table) {
                *op = table->owner_next;
                break;
            }
            op = &(*op)->owner_next;
        }
    }

    pthread_mutex_unlock(&g_ets_meta_lock);

    /* Free all entries */
    pthread_rwlock_wrlock(&table->rwlock);
    for (int i = 0; i < SW_ETS_BUCKETS; i++) {
        sw_ets_entry_t *e = table->buckets[i];
        while (e) {
            sw_ets_entry_t *next = e->next;
            free(e);
            e = next;
        }
    }
    pthread_rwlock_unlock(&table->rwlock);

    pthread_rwlock_destroy(&table->rwlock);
    free(table);
    return 0;
}

int sw_ets_info_count(sw_ets_tid_t tid) {
    sw_ets_table_t *table = ets_find_table(tid);
    if (!table) return -1;

    pthread_rwlock_rdlock(&table->rwlock);
    int count = (int)table->count;
    pthread_rwlock_unlock(&table->rwlock);

    return count;
}

int sw_ets_list_tids(uint32_t *out, int max) {
    int n = 0;
    pthread_mutex_lock(&g_ets_meta_lock);
    sw_ets_table_t *t = g_ets_tables;
    while (t && n < max) {
        out[n++] = t->tid;
        t = t->global_next;
    }
    pthread_mutex_unlock(&g_ets_meta_lock);
    return n;
}

void sw_ets_cleanup_owner(sw_process_t *proc) {
    /* Snapshot owned TIDs under meta lock */
    sw_ets_tid_t tids[256];
    int n = 0;

    pthread_mutex_lock(&g_ets_meta_lock);
    sw_ets_table_t *t = (sw_ets_table_t *)proc->ets_tables;
    while (t && n < 256) {
        tids[n++] = t->tid;
        t = t->owner_next;
    }
    pthread_mutex_unlock(&g_ets_meta_lock);

    /* Drop each table (sw_ets_drop re-acquires meta lock) */
    for (int i = 0; i < n; i++) {
        sw_ets_drop(tids[i]);
    }

    proc->ets_tables = NULL;
}
