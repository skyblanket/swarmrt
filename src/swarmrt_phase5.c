/*
 * SwarmRT Phase 5: GenStateMachine + Process Groups
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include "swarmrt_phase5.h"

/* ============================================================================
 * GENSTATEMACHINE IMPLEMENTATION
 *
 * A GenServer-like process that tracks a named state (int) alongside its data.
 * The single handle_event callback receives the event type, current state, and
 * returns the next state + any reply.
 * ============================================================================ */

/* Init data for the state machine process */
typedef struct {
    sw_sm_callbacks_t callbacks;
    void *init_arg;
    char name[SW_REG_NAME_MAX];
} sw_sm_init_t;

/* Internal: state timeout tag */
#define SW_TAG_SM_TIMEOUT 14

static void statemachine_entry(void *arg) {
    sw_sm_init_t *init = (sw_sm_init_t *)arg;

    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    if (init->name[0]) {
        sw_register(init->name, sw_self());
    }

    sw_sm_callbacks_t cbs = init->callbacks;
    int state = 0;
    void *data = cbs.init ? cbs.init(init->init_arg, &state) : init->init_arg;
    free(init);

    uint64_t timeout_ref = 0;
    int running = 1;

    while (running) {
        uint64_t tag = 0;
        void *msg = sw_receive_any((uint64_t)-1, &tag);

        if (!msg && tag == 0) break; /* Killed */

        sw_sm_event_type_t etype;
        void *event = NULL;
        sw_process_t *from = NULL;
        sw_gs_call_t *call = NULL;

        switch (tag) {
        case SW_TAG_CALL:
            call = (sw_gs_call_t *)msg;
            etype = SW_SM_CALL;
            event = call->payload;
            from = call->from;
            break;
        case SW_TAG_CAST: {
            sw_gs_cast_t *cast = (sw_gs_cast_t *)msg;
            etype = SW_SM_CAST;
            event = cast->payload;
            free(cast);
            msg = NULL; /* Already freed the wrapper */
            break;
        }
        case SW_TAG_STOP:
            if (msg) free(msg);
            running = 0;
            continue;
        case SW_TAG_TIMER:
            etype = SW_SM_TIMEOUT;
            event = msg;
            break;
        default:
            etype = SW_SM_INFO;
            event = msg;
            break;
        }

        if (!cbs.handle_event) {
            if (call) { free(call); }
            else if (msg) { free(msg); }
            continue;
        }

        sw_sm_result_t result = cbs.handle_event(etype, event, state, data, from);

        /* Apply result */
        data = result.new_data;

        switch (result.action) {
        case SW_SM_NEXT_STATE:
            state = result.next_state;
            break;
        case SW_SM_KEEP_STATE:
            break;
        case SW_SM_STOP:
            running = 0;
            break;
        }

        /* Reply to call if needed */
        if (call) {
            if (result.reply) {
                sw_send_tagged(call->from, call->ref, result.reply);
            } else {
                /* Must always reply to calls */
                sw_send_tagged(call->from, call->ref, NULL);
            }
            free(call);
        } else if (msg) {
            /* Already freed cast wrapper above, only free non-cast messages */
        }

        /* State timeout: only cancel on state change or new timeout.
         * A KEEP_STATE with timeout_ms=0 preserves the existing timer. */
        if (result.action == SW_SM_NEXT_STATE || result.timeout_ms > 0) {
            if (timeout_ref) {
                sw_cancel_timer(timeout_ref);
                timeout_ref = 0;
            }
        }
        if (result.timeout_ms > 0 && running) {
            timeout_ref = sw_send_after(result.timeout_ms, sw_self(),
                                        SW_TAG_TIMER, NULL);
        }
    }

    if (cbs.terminate) {
        cbs.terminate(state, data, 0);
    }
}

sw_process_t *sw_statemachine_start(const char *name, sw_sm_callbacks_t *cbs, void *arg) {
    sw_sm_init_t *init = (sw_sm_init_t *)malloc(sizeof(sw_sm_init_t));
    init->callbacks = *cbs;
    init->init_arg = arg;
    if (name)
        strncpy(init->name, name, SW_REG_NAME_MAX - 1);
    else
        init->name[0] = '\0';

    /* spawn_link + unlink to force different scheduler */
    sw_process_t *p = sw_spawn_link(statemachine_entry, init);
    if (p && sw_self()) sw_unlink(p);
    return p;
}

sw_process_t *sw_statemachine_start_link(const char *name, sw_sm_callbacks_t *cbs, void *arg) {
    sw_sm_init_t *init = (sw_sm_init_t *)malloc(sizeof(sw_sm_init_t));
    init->callbacks = *cbs;
    init->init_arg = arg;
    if (name)
        strncpy(init->name, name, SW_REG_NAME_MAX - 1);
    else
        init->name[0] = '\0';

    return sw_spawn_link(statemachine_entry, init);
}

/* Client API — reuses GenServer call/cast infrastructure */

void *sw_sm_call_proc(sw_process_t *sm, void *request, uint64_t timeout_ms) {
    return sw_call_proc(sm, request, timeout_ms);
}

void *sw_sm_call(const char *name, void *request, uint64_t timeout_ms) {
    sw_process_t *sm = sw_whereis(name);
    if (!sm) return NULL;
    return sw_sm_call_proc(sm, request, timeout_ms);
}

void sw_sm_cast_proc(sw_process_t *sm, void *event) {
    sw_cast_proc(sm, event);
}

void sw_sm_cast(const char *name, void *event) {
    sw_cast(name, event);
}

void sw_sm_stop(const char *name) {
    sw_send_named(name, SW_TAG_STOP, NULL);
}

/* ============================================================================
 * PROCESS GROUPS IMPLEMENTATION
 *
 * Global hash table of groups. Each group holds a dynamic array of members.
 * Protected by a global rwlock (reads are concurrent, writes exclusive).
 * ============================================================================ */

typedef struct sw_pg_member {
    sw_process_t *proc;
    struct sw_pg_member *next;
} sw_pg_member_t;

typedef struct sw_pg_group {
    char name[SW_REG_NAME_MAX];
    sw_pg_member_t *members;
    uint32_t count;
    struct sw_pg_group *next;  /* Hash chain */
} sw_pg_group_t;

static sw_pg_group_t *g_pg_buckets[SW_PG_BUCKETS];
static pthread_rwlock_t g_pg_lock = PTHREAD_RWLOCK_INITIALIZER;

static uint32_t pg_hash(const char *name) {
    uint32_t h = 5381;
    while (*name) {
        h = ((h << 5) + h) + (uint32_t)*name++;
    }
    return h % SW_PG_BUCKETS;
}

static sw_pg_group_t *pg_find(const char *name) {
    uint32_t idx = pg_hash(name);
    sw_pg_group_t *g = g_pg_buckets[idx];
    while (g) {
        if (strcmp(g->name, name) == 0) return g;
        g = g->next;
    }
    return NULL;
}

static sw_pg_group_t *pg_find_or_create(const char *name) {
    sw_pg_group_t *g = pg_find(name);
    if (g) return g;

    g = (sw_pg_group_t *)calloc(1, sizeof(sw_pg_group_t));
    strncpy(g->name, name, SW_REG_NAME_MAX - 1);

    uint32_t idx = pg_hash(name);
    g->next = g_pg_buckets[idx];
    g_pg_buckets[idx] = g;
    return g;
}

int sw_pg_join(const char *group, sw_process_t *proc) {
    if (!group || !proc) return -1;

    pthread_rwlock_wrlock(&g_pg_lock);

    sw_pg_group_t *g = pg_find_or_create(group);

    /* Check if already a member */
    sw_pg_member_t *m = g->members;
    while (m) {
        if (m->proc == proc) {
            pthread_rwlock_unlock(&g_pg_lock);
            return 0; /* Already joined */
        }
        m = m->next;
    }

    /* Add member */
    m = (sw_pg_member_t *)malloc(sizeof(sw_pg_member_t));
    m->proc = proc;
    m->next = g->members;
    g->members = m;
    g->count++;

    pthread_rwlock_unlock(&g_pg_lock);
    return 0;
}

int sw_pg_leave(const char *group, sw_process_t *proc) {
    if (!group || !proc) return -1;

    pthread_rwlock_wrlock(&g_pg_lock);

    sw_pg_group_t *g = pg_find(group);
    if (!g) {
        pthread_rwlock_unlock(&g_pg_lock);
        return -1;
    }

    sw_pg_member_t **pp = &g->members;
    while (*pp) {
        if ((*pp)->proc == proc) {
            sw_pg_member_t *found = *pp;
            *pp = found->next;
            free(found);
            g->count--;
            pthread_rwlock_unlock(&g_pg_lock);
            return 0;
        }
        pp = &(*pp)->next;
    }

    pthread_rwlock_unlock(&g_pg_lock);
    return -1;
}

int sw_pg_dispatch(const char *group, uint64_t tag, void *msg) {
    if (!group) return -1;

    pthread_rwlock_rdlock(&g_pg_lock);

    sw_pg_group_t *g = pg_find(group);
    if (!g) {
        pthread_rwlock_unlock(&g_pg_lock);
        return 0;
    }

    int sent = 0;
    sw_pg_member_t *m = g->members;
    while (m) {
        if (m->proc && m->proc->state != SW_PROC_FREE &&
            m->proc->state != SW_PROC_EXITING) {
            sw_send_tagged(m->proc, tag, msg);
            sent++;
        }
        m = m->next;
    }

    pthread_rwlock_unlock(&g_pg_lock);
    return sent;
}

int sw_pg_members(const char *group, sw_process_t **out, uint32_t max) {
    if (!group || !out) return 0;

    pthread_rwlock_rdlock(&g_pg_lock);

    sw_pg_group_t *g = pg_find(group);
    if (!g) {
        pthread_rwlock_unlock(&g_pg_lock);
        return 0;
    }

    uint32_t count = 0;
    sw_pg_member_t *m = g->members;
    while (m && count < max) {
        out[count++] = m->proc;
        m = m->next;
    }

    pthread_rwlock_unlock(&g_pg_lock);
    return (int)count;
}

uint32_t sw_pg_count(const char *group) {
    if (!group) return 0;

    pthread_rwlock_rdlock(&g_pg_lock);

    sw_pg_group_t *g = pg_find(group);
    uint32_t count = g ? g->count : 0;

    pthread_rwlock_unlock(&g_pg_lock);
    return count;
}

void sw_pg_cleanup_proc(sw_process_t *proc) {
    if (!proc) return;

    pthread_rwlock_wrlock(&g_pg_lock);

    for (int i = 0; i < SW_PG_BUCKETS; i++) {
        sw_pg_group_t *g = g_pg_buckets[i];
        while (g) {
            sw_pg_member_t **pp = &g->members;
            while (*pp) {
                if ((*pp)->proc == proc) {
                    sw_pg_member_t *found = *pp;
                    *pp = found->next;
                    free(found);
                    g->count--;
                } else {
                    pp = &(*pp)->next;
                }
            }
            g = g->next;
        }
    }

    pthread_rwlock_unlock(&g_pg_lock);
}
