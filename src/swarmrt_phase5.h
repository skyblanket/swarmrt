/*
 * SwarmRT Phase 5: GenStateMachine + Process Groups
 *
 * GenStateMachine: Finite state machine as a process. State transitions + events.
 * Process Groups:  Named groups of processes. Join/leave/dispatch.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_PHASE5_H
#define SWARMRT_PHASE5_H

#include "swarmrt_otp.h"

/* =========================================================================
 * GenStateMachine
 *
 * A process with named states + event-driven transitions.
 * Single callback: handle_event(type, event, state, data, from)
 *
 * Usage:
 *   sw_sm_callbacks_t cbs = { .init = my_init, .handle_event = my_handler };
 *   sw_statemachine_start("conn", &cbs, arg);
 *   void *r = sw_sm_call("conn", request, 5000);
 *   sw_sm_cast("conn", event);
 * ========================================================================= */

/* Event types */
typedef enum {
    SW_SM_CALL,        /* Synchronous call — must reply */
    SW_SM_CAST,        /* Async event */
    SW_SM_INFO,        /* System/other message */
    SW_SM_TIMEOUT,     /* State timeout fired */
} sw_sm_event_type_t;

/* Result action */
typedef enum {
    SW_SM_NEXT_STATE,  /* Transition to a new state */
    SW_SM_KEEP_STATE,  /* Stay in current state */
    SW_SM_STOP,        /* Terminate the state machine */
} sw_sm_action_t;

/* Result returned from handle_event */
typedef struct {
    sw_sm_action_t action;
    int next_state;        /* New state (for NEXT_STATE) */
    void *new_data;        /* Updated state data */
    void *reply;           /* Reply for calls (NULL = no reply) */
    uint64_t timeout_ms;   /* State timeout (0 = none) */
} sw_sm_result_t;

/* Callbacks */
typedef struct {
    void *(*init)(void *arg, int *initial_state);
    sw_sm_result_t (*handle_event)(sw_sm_event_type_t type, void *event,
                                   int state, void *data, sw_process_t *from);
    void (*terminate)(int state, void *data, int reason);
} sw_sm_callbacks_t;

/* Lifecycle */
sw_process_t *sw_statemachine_start(const char *name, sw_sm_callbacks_t *cbs, void *arg);
sw_process_t *sw_statemachine_start_link(const char *name, sw_sm_callbacks_t *cbs, void *arg);

/* Client API */
void *sw_sm_call(const char *name, void *request, uint64_t timeout_ms);
void *sw_sm_call_proc(sw_process_t *sm, void *request, uint64_t timeout_ms);
void  sw_sm_cast(const char *name, void *event);
void  sw_sm_cast_proc(sw_process_t *sm, void *event);
void  sw_sm_stop(const char *name);

/* =========================================================================
 * Process Groups (PG)
 *
 * Named groups. Multiple processes can join the same group.
 * Dispatch sends a message to all members.
 *
 * Usage:
 *   sw_pg_join("workers", sw_self());
 *   sw_pg_dispatch("workers", SW_TAG_CAST, work_msg);
 *   sw_pg_leave("workers", sw_self());
 * ========================================================================= */

#define SW_PG_MAX_MEMBERS 1024
#define SW_PG_BUCKETS     256

int  sw_pg_join(const char *group, sw_process_t *proc);
int  sw_pg_leave(const char *group, sw_process_t *proc);
int  sw_pg_dispatch(const char *group, uint64_t tag, void *msg);
int  sw_pg_members(const char *group, sw_process_t **out, uint32_t max);
uint32_t sw_pg_count(const char *group);

/* Called by process_exit to auto-remove dead process from all groups */
void sw_pg_cleanup_proc(sw_process_t *proc);

#endif /* SWARMRT_PHASE5_H */
