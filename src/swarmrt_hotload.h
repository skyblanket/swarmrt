/*
 * SwarmRT Phase 7: Hot Code Reload
 *
 * Swap the entry function of running processes at runtime.
 * Processes receive a SW_TAG_CODE_CHANGE message when their module is upgraded.
 * GenServer/StateMachine can handle this via handle_info to migrate state.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_HOTLOAD_H
#define SWARMRT_HOTLOAD_H

#include "swarmrt_native.h"

#define SW_TAG_CODE_CHANGE 15

/* Module: a named group of processes sharing the same entry function.
 * When a module is upgraded, all processes in it receive SW_TAG_CODE_CHANGE. */

#define SW_MODULE_NAME_MAX 64
#define SW_MODULE_MAX 256

typedef struct {
    char name[SW_MODULE_NAME_MAX];
    void (*current_func)(void *);
    void (*previous_func)(void *);  /* Old version kept for rollback */
    uint32_t version;
} sw_module_t;

/* Code change notification payload (delivered with SW_TAG_CODE_CHANGE) */
typedef struct {
    uint32_t old_version;
    uint32_t new_version;
    void (*new_func)(void *);
    char module_name[SW_MODULE_NAME_MAX];
} sw_code_change_t;

/* Register a module (group of processes sharing code) */
sw_module_t *sw_module_register(const char *name, void (*func)(void *));

/* Look up a module by name */
sw_module_t *sw_module_find(const char *name);

/* Upgrade a module to a new function. All processes spawned under this module
 * receive SW_TAG_CODE_CHANGE. GenServer handles it via handle_info. */
int sw_module_upgrade(const char *name, void (*new_func)(void *));

/* Rollback to previous version */
int sw_module_rollback(const char *name);

/* Get current version of a module */
uint32_t sw_module_version(const char *name);

/* Spawn a process under a module (tracks it for upgrade notifications) */
sw_process_t *sw_module_spawn(const char *module_name, void *arg);
sw_process_t *sw_module_spawn_link(const char *module_name, void *arg);

/* Internal: remove dead process from module tracking */
void sw_module_cleanup_proc(sw_process_t *proc);

#endif /* SWARMRT_HOTLOAD_H */
