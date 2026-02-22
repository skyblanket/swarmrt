/*
 * SwarmRT OTP Behaviours - GenServer + Supervisor
 *
 * Built on Phase 1 primitives: links, monitors, registry, tagged messages.
 * Pure C, no new scheduler changes needed.
 */

#ifndef SWARMRT_OTP_H
#define SWARMRT_OTP_H

#include "swarmrt_native.h"

/* =========================================================================
 * GenServer
 *
 * A process that:
 * 1. Maintains state across messages
 * 2. Handles sync calls (request → reply) via sw_call()
 * 3. Handles async casts (fire-and-forget) via sw_cast()
 * 4. Handles system messages (EXIT, DOWN, timer) via handle_info
 *
 * Usage:
 *   sw_gs_callbacks_t cbs = { .init = my_init, .handle_call = my_call, ... };
 *   sw_genserver_start_link("my_server", &cbs, init_arg);
 *   void *reply = sw_call("my_server", request, 5000);
 * ========================================================================= */

/* Call message payload (sent with tag=SW_TAG_CALL) */
typedef struct {
    uint64_t ref;          /* Unique call reference (also used as reply tag) */
    sw_process_t *from;    /* Caller process (for reply routing) */
    void *payload;         /* Request data */
} sw_gs_call_t;

/* Cast message payload (sent with tag=SW_TAG_CAST) */
typedef struct {
    void *payload;         /* Message data */
} sw_gs_cast_t;

/* Return type for handle_call */
typedef struct {
    void *reply;           /* Reply to send to caller */
    void *new_state;       /* Updated server state */
} sw_call_reply_t;

/* GenServer callbacks */
typedef struct {
    void *(*init)(void *arg);
    sw_call_reply_t (*handle_call)(void *state, sw_process_t *from, void *request);
    void *(*handle_cast)(void *state, void *message);
    void *(*handle_info)(void *state, uint64_t tag, void *info);
    void (*terminate)(void *state, int reason);
} sw_gs_callbacks_t;

/* Start a GenServer (standalone or linked to caller) */
sw_process_t *sw_genserver_start(const char *name, sw_gs_callbacks_t *cbs, void *init_arg);
sw_process_t *sw_genserver_start_link(const char *name, sw_gs_callbacks_t *cbs, void *init_arg);

/* Client API — call by name or by process pointer */
void *sw_call(const char *name, void *request, uint64_t timeout_ms);
void *sw_call_proc(sw_process_t *server, void *request, uint64_t timeout_ms);
void sw_cast(const char *name, void *message);
void sw_cast_proc(sw_process_t *server, void *message);

/* Stop a running GenServer */
void sw_genserver_stop(const char *name);

/* =========================================================================
 * Supervisor
 *
 * A process that:
 * 1. Starts child processes according to child specs
 * 2. Monitors all children (links + monitors)
 * 3. Restarts crashed children based on restart strategy
 * 4. Circuit breaker: crashes itself if too many restarts
 *
 * Usage:
 *   sw_child_spec_t children[] = {
 *       { "worker1", worker_fn, arg1, SW_PERMANENT },
 *       { "worker2", worker_fn, arg2, SW_TRANSIENT },
 *   };
 *   sw_sup_spec_t spec = {
 *       .strategy = SW_ONE_FOR_ONE,
 *       .max_restarts = 3, .max_seconds = 5,
 *       .children = children, .num_children = 2,
 *   };
 *   sw_supervisor_start_link("my_sup", &spec);
 * ========================================================================= */

/* Restart strategy */
typedef enum {
    SW_ONE_FOR_ONE,     /* Only restart the crashed child */
    SW_ONE_FOR_ALL,     /* Restart all children if one crashes */
    SW_REST_FOR_ONE,    /* Restart crashed child + all started after it */
} sw_restart_strategy_t;

/* Child restart policy */
typedef enum {
    SW_PERMANENT,       /* Always restart */
    SW_TEMPORARY,       /* Never restart */
    SW_TRANSIENT,       /* Restart only on abnormal exit (reason != 0) */
} sw_child_restart_t;

/* Child specification */
typedef struct {
    char name[SW_REG_NAME_MAX];
    void (*start_func)(void*);
    void *start_arg;
    sw_child_restart_t restart;
} sw_child_spec_t;

/* Supervisor specification */
#define SW_MAX_CHILDREN 64

typedef struct {
    sw_restart_strategy_t strategy;
    uint32_t max_restarts;       /* Max restarts within max_seconds */
    uint32_t max_seconds;        /* Time window for circuit breaker */
    sw_child_spec_t *children;
    uint32_t num_children;
} sw_sup_spec_t;

/* Start a Supervisor */
sw_process_t *sw_supervisor_start(const char *name, sw_sup_spec_t *spec);
sw_process_t *sw_supervisor_start_link(const char *name, sw_sup_spec_t *spec);

#endif /* SWARMRT_OTP_H */
