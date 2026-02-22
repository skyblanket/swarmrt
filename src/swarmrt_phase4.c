/*
 * SwarmRT Phase 4: Agent + Application + DynamicSupervisor
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include "swarmrt_phase4.h"

/* ============================================================================
 * AGENT IMPLEMENTATION
 *
 * Agent is a thin GenServer wrapper. The GenServer holds a single void* state.
 * get/get_and_update use call (synchronous), update uses cast (async).
 * ============================================================================ */

/* Internal: operation discriminator for call messages */
typedef enum {
    SW_AGENT_OP_GET = 0,
    SW_AGENT_OP_GAU = 1,
} sw_agent_call_op_t;

typedef struct {
    sw_agent_call_op_t op;
    union {
        sw_agent_get_fn get_fn;
        sw_agent_gau_fn gau_fn;
    };
    void *arg;
} sw_agent_call_msg_t;

typedef struct {
    sw_agent_update_fn update_fn;
    void *arg;
} sw_agent_cast_msg_t;

/* GenServer callbacks for Agent */

static void *agent_init(void *arg) {
    return arg; /* Initial state passed directly */
}

static sw_call_reply_t agent_handle_call(void *state, sw_process_t *from, void *request) {
    (void)from;
    sw_agent_call_msg_t *msg = (sw_agent_call_msg_t *)request;
    sw_call_reply_t result;

    switch (msg->op) {
    case SW_AGENT_OP_GET:
        result.reply = msg->get_fn(state, msg->arg);
        result.new_state = state;
        break;
    case SW_AGENT_OP_GAU: {
        sw_agent_gau_result_t gau = msg->gau_fn(state, msg->arg);
        result.reply = gau.reply;
        result.new_state = gau.new_state;
        break;
    }
    }

    return result;
}

static void *agent_handle_cast(void *state, void *message) {
    sw_agent_cast_msg_t *msg = (sw_agent_cast_msg_t *)message;
    void *new_state = msg->update_fn(state, msg->arg);
    return new_state;
}

static sw_gs_callbacks_t agent_callbacks = {
    .init = agent_init,
    .handle_call = agent_handle_call,
    .handle_cast = agent_handle_cast,
    .handle_info = NULL,
    .terminate = NULL,
};

/* --- Agent Lifecycle --- */

sw_process_t *sw_agent_start(const char *name, void *initial_state) {
    /* Use start_link + unlink to force different scheduler (avoids cooperative deadlock)
     * while keeping the agent unlinked from the caller */
    sw_process_t *agent = sw_genserver_start_link(name, &agent_callbacks, initial_state);
    if (agent && sw_self()) sw_unlink(agent);
    return agent;
}

sw_process_t *sw_agent_start_link(const char *name, void *initial_state) {
    return sw_genserver_start_link(name, &agent_callbacks, initial_state);
}

void sw_agent_stop(const char *name) {
    sw_genserver_stop(name);
}

void sw_agent_stop_proc(sw_process_t *agent) {
    if (agent) sw_send_tagged(agent, SW_TAG_STOP, NULL);
}

/* --- Agent Operations (by process) --- */

void *sw_agent_get_proc(sw_process_t *agent, sw_agent_get_fn func, void *arg, uint64_t timeout_ms) {
    sw_agent_call_msg_t *msg = (sw_agent_call_msg_t *)malloc(sizeof(sw_agent_call_msg_t));
    msg->op = SW_AGENT_OP_GET;
    msg->get_fn = func;
    msg->arg = arg;

    void *reply = sw_call_proc(agent, msg, timeout_ms);
    free(msg);
    return reply;
}

int sw_agent_update_proc(sw_process_t *agent, sw_agent_update_fn func, void *arg) {
    sw_agent_cast_msg_t *msg = (sw_agent_cast_msg_t *)malloc(sizeof(sw_agent_cast_msg_t));
    msg->update_fn = func;
    msg->arg = arg;

    sw_cast_proc(agent, msg);
    return 0;
}

void *sw_agent_get_and_update_proc(sw_process_t *agent, sw_agent_gau_fn func, void *arg, uint64_t timeout_ms) {
    sw_agent_call_msg_t *msg = (sw_agent_call_msg_t *)malloc(sizeof(sw_agent_call_msg_t));
    msg->op = SW_AGENT_OP_GAU;
    msg->gau_fn = func;
    msg->arg = arg;

    void *reply = sw_call_proc(agent, msg, timeout_ms);
    free(msg);
    return reply;
}

/* --- Agent Operations (by name) --- */

void *sw_agent_get(const char *name, sw_agent_get_fn func, void *arg, uint64_t timeout_ms) {
    sw_process_t *agent = sw_whereis(name);
    if (!agent) return NULL;
    return sw_agent_get_proc(agent, func, arg, timeout_ms);
}

int sw_agent_update(const char *name, sw_agent_update_fn func, void *arg) {
    sw_process_t *agent = sw_whereis(name);
    if (!agent) return -1;
    return sw_agent_update_proc(agent, func, arg);
}

void *sw_agent_get_and_update(const char *name, sw_agent_gau_fn func, void *arg, uint64_t timeout_ms) {
    sw_process_t *agent = sw_whereis(name);
    if (!agent) return NULL;
    return sw_agent_get_and_update_proc(agent, func, arg, timeout_ms);
}

/* ============================================================================
 * APPLICATION IMPLEMENTATION
 *
 * App controller: spawns root supervisor, waits for stop signal or supervisor
 * death, then tears down.
 * ============================================================================ */

typedef struct {
    char name[SW_REG_NAME_MAX];
    char sup_name[SW_REG_NAME_MAX];
    sw_sup_spec_t sup_spec;
    sw_child_spec_t children[SW_MAX_CHILDREN];
} sw_app_init_t;

static void app_controller_entry(void *arg) {
    sw_app_init_t *init = (sw_app_init_t *)arg;

    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    /* Register controller as "{name}_app" */
    char ctrl_name[SW_REG_NAME_MAX];
    snprintf(ctrl_name, sizeof(ctrl_name), "%s_app", init->name);
    sw_register(ctrl_name, sw_self());

    /* Save sup name before potential free */
    char sup_name[SW_REG_NAME_MAX];
    strncpy(sup_name, init->sup_name, SW_REG_NAME_MAX - 1);
    sup_name[SW_REG_NAME_MAX - 1] = '\0';

    /* Start root supervisor (linked).
     * NOTE: init is NOT freed — the supervisor runs on a different scheduler
     * and reads init->sup_spec.children asynchronously. Since applications
     * run for the program lifetime, this tiny leak is acceptable. */
    sw_process_t *root_sup = sw_supervisor_start_link(init->sup_name, &init->sup_spec);

    if (!root_sup) {
        free(init);
        return;
    }

    /* Wait for stop or supervisor death */
    while (1) {
        uint64_t tag = 0;
        void *msg = sw_receive_any((uint64_t)-1, &tag);

        if (tag == SW_TAG_STOP) {
            /* Forward stop to supervisor */
            sw_send_named(sup_name, SW_TAG_STOP, NULL);
            if (msg) free(msg);
            usleep(10000); /* Brief pause for supervisor cleanup */
            break;
        }
        if (tag == SW_TAG_EXIT) {
            /* Root supervisor died — we die too */
            if (msg) free(msg);
            break;
        }
        if (msg) free(msg);
    }
}

sw_process_t *sw_app_start(sw_app_spec_t *spec) {
    if (!spec || !spec->name) return NULL;

    sw_app_init_t *init = (sw_app_init_t *)calloc(1, sizeof(sw_app_init_t));
    strncpy(init->name, spec->name, SW_REG_NAME_MAX - 1);
    strncpy(init->sup_name, spec->name, SW_REG_NAME_MAX - 1);

    /* Copy child specs */
    uint32_t n = spec->num_children;
    if (n > SW_MAX_CHILDREN) n = SW_MAX_CHILDREN;
    for (uint32_t i = 0; i < n; i++) {
        init->children[i] = spec->children[i];
    }

    /* Build supervisor spec pointing to our copied children */
    init->sup_spec.strategy = spec->strategy;
    init->sup_spec.max_restarts = spec->max_restarts ? spec->max_restarts : 3;
    init->sup_spec.max_seconds = spec->max_seconds ? spec->max_seconds : 5;
    init->sup_spec.children = init->children;
    init->sup_spec.num_children = n;

    /* spawn_link + unlink to force different scheduler */
    sw_process_t *p = sw_spawn_link(app_controller_entry, init);
    if (p && sw_self()) sw_unlink(p);
    return p;
}

void sw_app_stop(const char *name) {
    if (!name) return;
    char ctrl_name[SW_REG_NAME_MAX];
    snprintf(ctrl_name, sizeof(ctrl_name), "%s_app", name);
    sw_send_named(ctrl_name, SW_TAG_STOP, NULL);
}

sw_process_t *sw_app_get_supervisor(const char *name) {
    if (!name) return NULL;
    return sw_whereis(name);
}

/* ============================================================================
 * DYNAMIC SUPERVISOR IMPLEMENTATION
 *
 * Process loop that manages a linked list of children. Children are added at
 * runtime via call (SW_TAG_CALL). Each child is spawn_linked + monitored.
 * On DOWN, applies restart policy. Circuit breaker on too many restarts.
 * ============================================================================ */

/* Internal child node */
typedef struct sw_dynsup_child {
    sw_child_spec_t spec;
    sw_process_t *proc;
    uint64_t monitor_ref;
    struct sw_dynsup_child *next;
} sw_dynsup_child_t;

/* Operation discriminator */
typedef enum {
    SW_DYNSUP_OP_START_CHILD,
    SW_DYNSUP_OP_TERM_CHILD,
    SW_DYNSUP_OP_COUNT,
} sw_dynsup_op_t;

/* Request payload (sent as call->payload) */
typedef struct {
    sw_dynsup_op_t op;
    union {
        sw_child_spec_t child_spec;   /* START_CHILD */
        sw_process_t *child;          /* TERM_CHILD */
    };
} sw_dynsup_request_t;

/* Supervisor runtime state */
typedef struct {
    sw_dynsup_spec_t spec;
    char name[SW_REG_NAME_MAX];
    sw_dynsup_child_t *children;
    uint32_t child_count;
    uint32_t restart_times[64];
    uint32_t restart_idx;
    uint32_t restart_count;
} sw_dynsup_state_t;

static uint32_t dynsup_now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec;
}

static int dynsup_check_circuit_breaker(sw_dynsup_state_t *st) {
    if (st->spec.max_restarts == 0) return 0;

    uint32_t now = dynsup_now_seconds();
    uint32_t window = st->spec.max_seconds;
    if (window == 0) window = 1;

    uint32_t count = 0;
    for (uint32_t i = 0; i < st->restart_count && i < 64; i++) {
        uint32_t idx = (st->restart_idx - 1 - i + 64) % 64;
        if (now - st->restart_times[idx] <= window) {
            count++;
        } else {
            break;
        }
    }
    return count >= st->spec.max_restarts;
}

static void dynsup_record_restart(sw_dynsup_state_t *st) {
    st->restart_times[st->restart_idx] = dynsup_now_seconds();
    st->restart_idx = (st->restart_idx + 1) % 64;
    if (st->restart_count < 64) st->restart_count++;
}

static void dynsup_kill_child(sw_dynsup_child_t *child) {
    if (!child->proc) return;

    sw_demonitor(child->monitor_ref);
    sw_unlink(child->proc);

    sw_process_kill(child->proc, -1);
    child->proc = NULL;
}

static void dynsup_kill_all(sw_dynsup_state_t *st) {
    sw_dynsup_child_t *c = st->children;
    while (c) {
        sw_dynsup_child_t *next = c->next;
        if (c->proc) dynsup_kill_child(c);
        free(c);
        c = next;
    }
    st->children = NULL;
    st->child_count = 0;
}

/* Remove a child node from the linked list (by proc pointer) */
static sw_dynsup_child_t *dynsup_remove_child(sw_dynsup_state_t *st, sw_process_t *proc) {
    sw_dynsup_child_t **pp = &st->children;
    while (*pp) {
        if ((*pp)->proc == proc) {
            sw_dynsup_child_t *found = *pp;
            *pp = found->next;
            st->child_count--;
            return found;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

/* Find child by monitor ref */
static sw_dynsup_child_t *dynsup_find_by_ref(sw_dynsup_state_t *st, uint64_t ref) {
    sw_dynsup_child_t *c = st->children;
    while (c) {
        if (c->monitor_ref == ref) return c;
        c = c->next;
    }
    return NULL;
}

/* Remove child node from list by monitor ref */
static sw_dynsup_child_t *dynsup_remove_by_ref(sw_dynsup_state_t *st, uint64_t ref) {
    sw_dynsup_child_t **pp = &st->children;
    while (*pp) {
        if ((*pp)->monitor_ref == ref) {
            sw_dynsup_child_t *found = *pp;
            *pp = found->next;
            st->child_count--;
            return found;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

/* Init data for the DynamicSupervisor process */
typedef struct {
    sw_dynsup_spec_t spec;
    char name[SW_REG_NAME_MAX];
} sw_dynsup_init_t;

static void dynsup_entry(void *arg) {
    sw_dynsup_init_t *init = (sw_dynsup_init_t *)arg;

    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    if (init->name[0]) {
        sw_register(init->name, sw_self());
    }

    sw_dynsup_state_t st;
    memset(&st, 0, sizeof(st));
    st.spec = init->spec;
    strncpy(st.name, init->name, SW_REG_NAME_MAX - 1);
    free(init);

    while (1) {
        uint64_t tag = 0;
        void *msg = sw_receive_any((uint64_t)-1, &tag);

        if (!msg && tag == 0) break; /* Killed */

        if (tag == SW_TAG_CALL) {
            sw_gs_call_t *call = (sw_gs_call_t *)msg;
            sw_dynsup_request_t *req = (sw_dynsup_request_t *)call->payload;

            switch (req->op) {
            case SW_DYNSUP_OP_START_CHILD: {
                /* Check max children */
                uint32_t max = st.spec.max_children;
                if (max == 0) max = SW_DYNSUP_MAX_CHILDREN;
                if (st.child_count >= max) {
                    sw_send_tagged(call->from, call->ref, NULL);
                    break;
                }

                /* Spawn linked child */
                sw_process_t *child = sw_spawn_link(req->child_spec.start_func,
                                                      req->child_spec.start_arg);
                if (!child) {
                    sw_send_tagged(call->from, call->ref, NULL);
                    break;
                }

                /* Monitor */
                uint64_t mref = sw_monitor(child);

                /* Register name if provided */
                if (req->child_spec.name[0]) {
                    sw_register(req->child_spec.name, child);
                }

                /* Add to child list */
                sw_dynsup_child_t *node = (sw_dynsup_child_t *)calloc(1, sizeof(sw_dynsup_child_t));
                node->spec = req->child_spec;
                node->proc = child;
                node->monitor_ref = mref;
                node->next = st.children;
                st.children = node;
                st.child_count++;

                /* Reply with child pointer */
                sw_send_tagged(call->from, call->ref, child);
                break;
            }
            case SW_DYNSUP_OP_TERM_CHILD: {
                sw_dynsup_child_t *node = dynsup_remove_child(&st, req->child);
                if (node) {
                    dynsup_kill_child(node);
                    free(node);
                    /* Reply success (non-NULL) */
                    sw_send_tagged(call->from, call->ref, (void *)1);
                } else {
                    sw_send_tagged(call->from, call->ref, NULL);
                }
                break;
            }
            case SW_DYNSUP_OP_COUNT:
                /* Reply with count cast to void* */
                sw_send_tagged(call->from, call->ref, (void *)(uintptr_t)st.child_count);
                break;
            }

            free(req);
            free(call);

        } else if (tag == SW_TAG_DOWN) {
            sw_signal_t *sig = (sw_signal_t *)msg;

            sw_dynsup_child_t *child = dynsup_find_by_ref(&st, sig->ref);
            if (child) {
                int reason = sig->reason;
                child->proc = NULL;

                /* Decide restart */
                int should_restart = 0;
                switch (child->spec.restart) {
                case SW_PERMANENT:
                    should_restart = 1;
                    break;
                case SW_TRANSIENT:
                    should_restart = (reason != 0);
                    break;
                case SW_TEMPORARY:
                    should_restart = 0;
                    break;
                }

                if (should_restart) {
                    if (dynsup_check_circuit_breaker(&st)) {
                        fprintf(stderr,
                            "[DynSup] Max restarts (%u/%us) exceeded — shutting down\n",
                            st.spec.max_restarts, st.spec.max_seconds);
                        dynsup_kill_all(&st);
                        sw_self()->exit_reason = -2;
                        free(msg);
                        return;
                    }

                    dynsup_record_restart(&st);

                    /* Restart: spawn new, replace in node */
                    sw_process_t *new_child = sw_spawn_link(child->spec.start_func,
                                                              child->spec.start_arg);
                    if (new_child) {
                        child->proc = new_child;
                        child->monitor_ref = sw_monitor(new_child);
                        if (child->spec.name[0]) {
                            sw_register(child->spec.name, new_child);
                        }
                    } else {
                        /* Spawn failed — remove from list */
                        dynsup_remove_by_ref(&st, sig->ref);
                        free(child);
                    }
                } else {
                    /* No restart — remove from list */
                    dynsup_remove_by_ref(&st, sig->ref);
                    free(child);
                }
            }

            free(msg);

        } else if (tag == SW_TAG_EXIT) {
            /* Absorbed (we trap exits) */
            free(msg);

        } else if (tag == SW_TAG_STOP) {
            if (msg) free(msg);
            dynsup_kill_all(&st);
            break;

        } else {
            if (msg) free(msg);
        }
    }
}

/* --- DynamicSupervisor Lifecycle --- */

sw_process_t *sw_dynsup_start(const char *name, sw_dynsup_spec_t *spec) {
    sw_dynsup_init_t *init = (sw_dynsup_init_t *)malloc(sizeof(sw_dynsup_init_t));
    init->spec = *spec;
    if (name)
        strncpy(init->name, name, SW_REG_NAME_MAX - 1);
    else
        init->name[0] = '\0';

    /* spawn_link + unlink to force different scheduler without linking */
    sw_process_t *p = sw_spawn_link(dynsup_entry, init);
    if (p && sw_self()) sw_unlink(p);
    return p;
}

sw_process_t *sw_dynsup_start_link(const char *name, sw_dynsup_spec_t *spec) {
    sw_dynsup_init_t *init = (sw_dynsup_init_t *)malloc(sizeof(sw_dynsup_init_t));
    init->spec = *spec;
    if (name)
        strncpy(init->name, name, SW_REG_NAME_MAX - 1);
    else
        init->name[0] = '\0';

    return sw_spawn_link(dynsup_entry, init);
}

/* --- DynamicSupervisor Client API --- */

sw_process_t *sw_dynsup_start_child_proc(sw_process_t *sup, sw_child_spec_t *child_spec) {
    if (!sup || !child_spec) return NULL;

    sw_dynsup_request_t *req = (sw_dynsup_request_t *)malloc(sizeof(sw_dynsup_request_t));
    req->op = SW_DYNSUP_OP_START_CHILD;
    req->child_spec = *child_spec;

    void *reply = sw_call_proc(sup, req, 5000);
    /* Note: req is freed by the DynSup process after handling */
    return (sw_process_t *)reply;
}

sw_process_t *sw_dynsup_start_child(const char *sup_name, sw_child_spec_t *child_spec) {
    sw_process_t *sup = sw_whereis(sup_name);
    if (!sup) return NULL;
    return sw_dynsup_start_child_proc(sup, child_spec);
}

int sw_dynsup_terminate_child_proc(sw_process_t *sup, sw_process_t *child) {
    if (!sup || !child) return -1;

    sw_dynsup_request_t *req = (sw_dynsup_request_t *)malloc(sizeof(sw_dynsup_request_t));
    req->op = SW_DYNSUP_OP_TERM_CHILD;
    req->child = child;

    void *reply = sw_call_proc(sup, req, 5000);
    return reply ? 0 : -1;
}

int sw_dynsup_terminate_child(const char *sup_name, sw_process_t *child) {
    sw_process_t *sup = sw_whereis(sup_name);
    if (!sup) return -1;
    return sw_dynsup_terminate_child_proc(sup, child);
}

uint32_t sw_dynsup_count_children_proc(sw_process_t *sup) {
    if (!sup) return 0;

    sw_dynsup_request_t *req = (sw_dynsup_request_t *)malloc(sizeof(sw_dynsup_request_t));
    req->op = SW_DYNSUP_OP_COUNT;

    void *reply = sw_call_proc(sup, req, 5000);
    return (uint32_t)(uintptr_t)reply;
}

uint32_t sw_dynsup_count_children(const char *sup_name) {
    sw_process_t *sup = sw_whereis(sup_name);
    if (!sup) return 0;
    return sw_dynsup_count_children_proc(sup);
}
