/*
 * SwarmRT Behaviours - GenServer + Supervisor Implementation
 *
 * GenServer: callback-based server with call/cast/info.
 * Supervisor: fault-tolerant child process management.
 *
 * Both are regular SwarmRT processes using Phase 1 primitives:
 * links, monitors, registry, tagged messages, timers.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include "swarmrt_otp.h"

/* Call ref counter — starts at 1000 to avoid collision with system tags */
static _Atomic uint64_t g_next_call_ref = 1000;

/* ============================================================================
 * GENSERVER IMPLEMENTATION
 * ============================================================================ */

/* Init data passed as arg to the genserver process entry function */
typedef struct {
    sw_gs_callbacks_t callbacks;
    void *init_arg;
    char name[SW_REG_NAME_MAX];
    int link_parent;
} sw_gs_init_t;

/*
 * GenServer process entry function.
 * Runs the init → loop → terminate lifecycle.
 */
static void genserver_entry(void *arg) {
    sw_gs_init_t *init = (sw_gs_init_t *)arg;

    /* Trap exits for clean shutdown */
    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    /* Register if name provided */
    if (init->name[0]) {
        sw_register(init->name, sw_self());
    }

    /* Call init callback */
    sw_gs_callbacks_t cbs = init->callbacks;
    void *state = cbs.init ? cbs.init(init->init_arg) : init->init_arg;
    free(init);

    /* Message loop */
    int running = 1;
    while (running) {
        uint64_t tag = 0;
        void *msg = sw_receive_any((uint64_t)-1, &tag);

        if (!msg && tag == 0) break; /* Killed */

        switch (tag) {
        case SW_TAG_CALL: {
            sw_gs_call_t *call = (sw_gs_call_t *)msg;
            if (cbs.handle_call) {
                sw_call_reply_t result = cbs.handle_call(state, call->from, call->payload);
                state = result.new_state;
                /* Reply with tag = call ref (unique per call) */
                sw_send_tagged(call->from, call->ref, result.reply);
            }
            free(call);
            break;
        }
        case SW_TAG_CAST: {
            sw_gs_cast_t *cast = (sw_gs_cast_t *)msg;
            if (cbs.handle_cast) {
                state = cbs.handle_cast(state, cast->payload);
            }
            free(cast);
            break;
        }
        case SW_TAG_STOP:
            if (msg) free(msg);
            running = 0;
            break;
        case SW_TAG_EXIT:
        case SW_TAG_DOWN:
        case SW_TAG_TIMER:
        default:
            if (cbs.handle_info) {
                state = cbs.handle_info(state, tag, msg);
            } else {
                if (msg) free(msg);
            }
            break;
        }
    }

    /* Terminate */
    if (cbs.terminate) {
        cbs.terminate(state, 0);
    }
}

sw_process_t *sw_genserver_start(const char *name, sw_gs_callbacks_t *cbs, void *init_arg) {
    sw_gs_init_t *init = (sw_gs_init_t *)malloc(sizeof(sw_gs_init_t));
    init->callbacks = *cbs;
    init->init_arg = init_arg;
    init->link_parent = 0;
    if (name)
        strncpy(init->name, name, SW_REG_NAME_MAX - 1);
    else
        init->name[0] = '\0';

    return sw_spawn(genserver_entry, init);
}

sw_process_t *sw_genserver_start_link(const char *name, sw_gs_callbacks_t *cbs, void *init_arg) {
    sw_gs_init_t *init = (sw_gs_init_t *)malloc(sizeof(sw_gs_init_t));
    init->callbacks = *cbs;
    init->init_arg = init_arg;
    init->link_parent = 1;
    if (name)
        strncpy(init->name, name, SW_REG_NAME_MAX - 1);
    else
        init->name[0] = '\0';

    return sw_spawn_link(genserver_entry, init);
}

/* --- Client API --- */

void *sw_call_proc(sw_process_t *server, void *request, uint64_t timeout_ms) {
    if (!server) return NULL;

    uint64_t ref = atomic_fetch_add(&g_next_call_ref, 1);

    sw_gs_call_t *call = (sw_gs_call_t *)malloc(sizeof(sw_gs_call_t));
    call->ref = ref;
    call->from = sw_self();
    call->payload = request;

    sw_send_tagged(server, SW_TAG_CALL, call);

    /* Wait for reply tagged with our unique ref */
    return sw_receive_tagged(ref, timeout_ms);
}

void *sw_call(const char *name, void *request, uint64_t timeout_ms) {
    sw_process_t *server = sw_whereis(name);
    if (!server) return NULL;
    return sw_call_proc(server, request, timeout_ms);
}

void sw_cast_proc(sw_process_t *server, void *message) {
    if (!server) return;

    sw_gs_cast_t *cast = (sw_gs_cast_t *)malloc(sizeof(sw_gs_cast_t));
    cast->payload = message;

    sw_send_tagged(server, SW_TAG_CAST, cast);
}

void sw_cast(const char *name, void *message) {
    sw_process_t *server = sw_whereis(name);
    if (!server) return;
    sw_cast_proc(server, message);
}

void sw_genserver_stop(const char *name) {
    sw_send_named(name, SW_TAG_STOP, NULL);
}

/* ============================================================================
 * SUPERVISOR IMPLEMENTATION
 * ============================================================================ */

/* Per-child runtime state (internal to supervisor) */
typedef struct {
    sw_child_spec_t spec;
    sw_process_t *proc;
    uint64_t monitor_ref;
    int alive;
} sw_sup_child_rt_t;

/* Supervisor runtime state */
typedef struct {
    sw_sup_spec_t spec;
    sw_sup_child_rt_t children[SW_MAX_CHILDREN];
    uint32_t restart_times[64]; /* Ring buffer of restart timestamps */
    uint32_t restart_idx;
    uint32_t restart_count;
} sw_sup_state_t;

static uint32_t now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec;
}

static int sup_check_circuit_breaker(sw_sup_state_t *st) {
    if (st->spec.max_restarts == 0) return 0; /* No limit */

    uint32_t now = now_seconds();
    uint32_t window = st->spec.max_seconds;
    if (window == 0) window = 1;

    /* Count restarts within the time window */
    uint32_t count = 0;
    for (uint32_t i = 0; i < st->restart_count && i < 64; i++) {
        uint32_t idx = (st->restart_idx - 1 - i + 64) % 64;
        if (now - st->restart_times[idx] <= window) {
            count++;
        } else {
            break; /* Older entries are outside window */
        }
    }

    return count >= st->spec.max_restarts;
}

static void sup_record_restart(sw_sup_state_t *st) {
    st->restart_times[st->restart_idx] = now_seconds();
    st->restart_idx = (st->restart_idx + 1) % 64;
    if (st->restart_count < 64) st->restart_count++;
}

static int sup_start_child(sw_sup_child_rt_t *child) {
    child->proc = sw_spawn_link(child->spec.start_func, child->spec.start_arg);
    if (!child->proc) return -1;

    child->monitor_ref = sw_monitor(child->proc);
    child->alive = 1;

    if (child->spec.name[0]) {
        sw_register(child->spec.name, child->proc);
    }

    return 0;
}

static void sup_kill_child(sw_sup_child_rt_t *child) {
    if (!child->alive || !child->proc) return;

    /* Demonitor + unlink before killing to avoid recursive signals */
    sw_demonitor(child->monitor_ref);
    sw_unlink(child->proc);

    /* Kill the child process (lock-free) */
    sw_process_kill(child->proc, -1);

    child->alive = 0;
    child->proc = NULL;
}

/* Init data passed to supervisor process */
typedef struct {
    sw_sup_spec_t spec;
    char name[SW_REG_NAME_MAX];
} sw_sup_init_t;

static void supervisor_entry(void *arg) {
    sw_sup_init_t *init = (sw_sup_init_t *)arg;

    /* Trap exits */
    sw_process_flag(SW_FLAG_TRAP_EXIT, 1);

    /* Register */
    if (init->name[0]) {
        sw_register(init->name, sw_self());
    }

    /* Initialize state on the HEAP (sw_sup_state_t is ~7.5KB,
     * process stacks are only 8KB — stack-allocating this overflows) */
    sw_sup_state_t *st = (sw_sup_state_t *)calloc(1, sizeof(sw_sup_state_t));
    st->spec = init->spec;

    /* Copy child specs */
    for (uint32_t i = 0; i < init->spec.num_children && i < SW_MAX_CHILDREN; i++) {
        st->children[i].spec = init->spec.children[i];
        st->children[i].alive = 0;
    }

    free(init);

    /* Start all children */
    for (uint32_t i = 0; i < st->spec.num_children; i++) {
        if (sup_start_child(&st->children[i]) != 0) {
            fprintf(stderr, "[Supervisor] Failed to start child '%s'\n",
                    st->children[i].spec.name);
        }
    }

    /* Monitor loop */
    while (1) {
        uint64_t tag = 0;
        void *msg = sw_receive_any((uint64_t)-1, &tag);

        if (!msg && tag == 0) break; /* Killed */

        if (tag == SW_TAG_DOWN) {
            sw_signal_t *sig = (sw_signal_t *)msg;

            /* Find which child died */
            int child_idx = -1;
            for (uint32_t i = 0; i < st->spec.num_children; i++) {
                if (st->children[i].alive &&
                    st->children[i].monitor_ref == sig->ref) {
                    child_idx = (int)i;
                    break;
                }
            }

            if (child_idx >= 0) {
                sw_sup_child_rt_t *child = &st->children[child_idx];
                int reason = sig->reason;
                child->alive = 0;
                child->proc = NULL;

                /* Decide whether to restart */
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
                    /* Check circuit breaker BEFORE recording this restart
                     * (so max_restarts=3 allows 3 actual restarts = 4 total starts) */
                    if (sup_check_circuit_breaker(st)) {
                        fprintf(stderr,
                            "[Supervisor] Max restarts (%u/%us) exceeded — shutting down\n",
                            st->spec.max_restarts, st->spec.max_seconds);
                        /* Kill all remaining children and exit */
                        for (uint32_t i = 0; i < st->spec.num_children; i++) {
                            sup_kill_child(&st->children[i]);
                        }
                        sw_self()->exit_reason = -2; /* shutdown */
                        free(msg);
                        free(st);
                        return;
                    }

                    sup_record_restart(st);

                    /* Apply restart strategy */
                    switch (st->spec.strategy) {
                    case SW_ONE_FOR_ONE:
                        /* Restart only the crashed child */
                        sup_start_child(child);
                        break;

                    case SW_ONE_FOR_ALL:
                        /* Kill all, restart all */
                        for (uint32_t i = 0; i < st->spec.num_children; i++) {
                            if ((int)i != child_idx && st->children[i].alive) {
                                sup_kill_child(&st->children[i]);
                            }
                        }
                        usleep(1000); /* Brief pause for cleanup */
                        for (uint32_t i = 0; i < st->spec.num_children; i++) {
                            if (!st->children[i].alive) {
                                sup_start_child(&st->children[i]);
                            }
                        }
                        break;

                    case SW_REST_FOR_ONE:
                        /* Kill children after the crashed one, restart them */
                        for (uint32_t i = (uint32_t)child_idx + 1;
                             i < st->spec.num_children; i++) {
                            if (st->children[i].alive) {
                                sup_kill_child(&st->children[i]);
                            }
                        }
                        usleep(1000);
                        for (uint32_t i = (uint32_t)child_idx;
                             i < st->spec.num_children; i++) {
                            if (!st->children[i].alive) {
                                sup_start_child(&st->children[i]);
                            }
                        }
                        break;
                    }
                }
            }

            free(msg);

        } else if (tag == SW_TAG_EXIT) {
            /* Linked process exit — handled via monitors above */
            free(msg);
        } else if (tag == SW_TAG_STOP) {
            /* Graceful shutdown */
            if (msg) free(msg);
            for (uint32_t i = 0; i < st->spec.num_children; i++) {
                sup_kill_child(&st->children[i]);
            }
            break;
        } else {
            if (msg) free(msg);
        }
    }

    free(st);
}

sw_process_t *sw_supervisor_start(const char *name, sw_sup_spec_t *spec) {
    sw_sup_init_t *init = (sw_sup_init_t *)malloc(sizeof(sw_sup_init_t));
    init->spec = *spec;
    if (name)
        strncpy(init->name, name, SW_REG_NAME_MAX - 1);
    else
        init->name[0] = '\0';

    return sw_spawn(supervisor_entry, init);
}

sw_process_t *sw_supervisor_start_link(const char *name, sw_sup_spec_t *spec) {
    sw_sup_init_t *init = (sw_sup_init_t *)malloc(sizeof(sw_sup_init_t));
    init->spec = *spec;
    if (name)
        strncpy(init->name, name, SW_REG_NAME_MAX - 1);
    else
        init->name[0] = '\0';

    return sw_spawn_link(supervisor_entry, init);
}
