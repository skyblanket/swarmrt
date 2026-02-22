/*
 * SwarmRT Phase 4: Agent + Application + DynamicSupervisor
 *
 * Agent:             Thin state wrapper over GenServer. get/update/get_and_update.
 * Application:       Top-level supervision tree entry point. start/stop.
 * DynamicSupervisor: Zero-child supervisor. Children added at runtime.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_PHASE4_H
#define SWARMRT_PHASE4_H

#include "swarmrt_otp.h"

/* =========================================================================
 * Agent
 *
 * Simpler than GenServer — just holds state with get/update/get_and_update.
 * Uses GenServer internally.
 *
 * Usage:
 *   sw_agent_start("config", initial_state);
 *   void *val = sw_agent_get("config", getter_fn, NULL, 5000);
 *   sw_agent_update("config", updater_fn, new_data);
 *   sw_agent_stop("config");
 * ========================================================================= */

typedef void *(*sw_agent_get_fn)(void *state, void *arg);
typedef void *(*sw_agent_update_fn)(void *state, void *arg);

typedef struct {
    void *reply;
    void *new_state;
} sw_agent_gau_result_t;

typedef sw_agent_gau_result_t (*sw_agent_gau_fn)(void *state, void *arg);

/* Lifecycle */
sw_process_t *sw_agent_start(const char *name, void *initial_state);
sw_process_t *sw_agent_start_link(const char *name, void *initial_state);
void sw_agent_stop(const char *name);
void sw_agent_stop_proc(sw_process_t *agent);

/* Operations by name */
void *sw_agent_get(const char *name, sw_agent_get_fn func, void *arg, uint64_t timeout_ms);
int   sw_agent_update(const char *name, sw_agent_update_fn func, void *arg);
void *sw_agent_get_and_update(const char *name, sw_agent_gau_fn func, void *arg, uint64_t timeout_ms);

/* Operations by process pointer */
void *sw_agent_get_proc(sw_process_t *agent, sw_agent_get_fn func, void *arg, uint64_t timeout_ms);
int   sw_agent_update_proc(sw_process_t *agent, sw_agent_update_fn func, void *arg);
void *sw_agent_get_and_update_proc(sw_process_t *agent, sw_agent_gau_fn func, void *arg, uint64_t timeout_ms);

/* =========================================================================
 * Application
 *
 * Top-level entry point. Starts a root supervisor with children.
 *
 * Usage:
 *   sw_child_spec_t children[] = { ... };
 *   sw_app_spec_t spec = { .name = "myapp", .children = children, .num_children = 2 };
 *   sw_app_start(&spec);
 *   // ... runtime ...
 *   sw_app_stop("myapp");
 * ========================================================================= */

typedef struct {
    const char *name;
    sw_child_spec_t *children;
    uint32_t num_children;
    sw_restart_strategy_t strategy;
    uint32_t max_restarts;
    uint32_t max_seconds;
} sw_app_spec_t;

sw_process_t *sw_app_start(sw_app_spec_t *spec);
void sw_app_stop(const char *name);
sw_process_t *sw_app_get_supervisor(const char *name);

/* =========================================================================
 * DynamicSupervisor
 *
 * Starts with zero children. Add/remove at runtime.
 * Only supports one_for_one (each child independent).
 *
 * Usage:
 *   sw_dynsup_spec_t spec = { .max_restarts = 5, .max_seconds = 10 };
 *   sw_dynsup_start("sessions", &spec);
 *   sw_process_t *child = sw_dynsup_start_child("sessions", &child_spec);
 *   sw_dynsup_terminate_child("sessions", child);
 * ========================================================================= */

#define SW_DYNSUP_MAX_CHILDREN 4096

typedef struct {
    uint32_t max_restarts;
    uint32_t max_seconds;
    uint32_t max_children;   /* 0 = unlimited (up to SW_DYNSUP_MAX_CHILDREN) */
} sw_dynsup_spec_t;

sw_process_t *sw_dynsup_start(const char *name, sw_dynsup_spec_t *spec);
sw_process_t *sw_dynsup_start_link(const char *name, sw_dynsup_spec_t *spec);

sw_process_t *sw_dynsup_start_child(const char *sup_name, sw_child_spec_t *child_spec);
sw_process_t *sw_dynsup_start_child_proc(sw_process_t *sup, sw_child_spec_t *child_spec);

int sw_dynsup_terminate_child(const char *sup_name, sw_process_t *child);
int sw_dynsup_terminate_child_proc(sw_process_t *sup, sw_process_t *child);

uint32_t sw_dynsup_count_children(const char *sup_name);
uint32_t sw_dynsup_count_children_proc(sw_process_t *sup);

#endif /* SWARMRT_PHASE4_H */
