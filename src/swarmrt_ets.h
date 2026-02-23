/*
 * SwarmRT ETS - Concurrent In-Memory Tables
 *
 * Hash table with per-table rwlock for concurrent reads, exclusive writes.
 * Owner process tracking with auto-cleanup on exit.
 * Access modes: public, protected, private.
 */

#ifndef SWARMRT_ETS_H
#define SWARMRT_ETS_H

#include "swarmrt_native.h"

typedef enum { SW_ETS_SET, SW_ETS_ORDERED_SET, SW_ETS_BAG } sw_ets_type_t;
typedef enum { SW_ETS_PUBLIC, SW_ETS_PROTECTED, SW_ETS_PRIVATE } sw_ets_access_t;

typedef struct {
    sw_ets_type_t type;
    sw_ets_access_t access;
} sw_ets_opts_t;

#define SW_ETS_DEFAULT ((sw_ets_opts_t){ SW_ETS_SET, SW_ETS_PUBLIC })
#define SW_ETS_INVALID_TID 0

typedef uint32_t sw_ets_tid_t;

sw_ets_tid_t sw_ets_new(sw_ets_opts_t opts);
int sw_ets_insert(sw_ets_tid_t tid, void *key, void *value);
void *sw_ets_lookup(sw_ets_tid_t tid, void *key);
int sw_ets_delete(sw_ets_tid_t tid, void *key);
int sw_ets_drop(sw_ets_tid_t tid);
int sw_ets_info_count(sw_ets_tid_t tid);

/* Called by process_exit to clean up owned tables */
void sw_ets_cleanup_owner(sw_process_t *proc);

#endif /* SWARMRT_ETS_H */
