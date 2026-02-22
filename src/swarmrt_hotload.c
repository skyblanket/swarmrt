/*
 * SwarmRT Phase 7: Hot Code Reload
 *
 * Modules track groups of processes. When upgraded, all tracked processes
 * receive a SW_TAG_CODE_CHANGE notification with the new version number.
 * The process can then migrate state as needed.
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "swarmrt_hotload.h"

/* === Module Registry === */

/* Tracked process list per module */
typedef struct sw_mod_proc {
    sw_process_t *proc;
    struct sw_mod_proc *next;
} sw_mod_proc_t;

typedef struct {
    sw_module_t info;
    sw_mod_proc_t *procs;
    uint32_t proc_count;
    int active;
} sw_mod_entry_t;

static sw_mod_entry_t g_modules[SW_MODULE_MAX];
static uint32_t g_module_count = 0;
static pthread_rwlock_t g_module_lock = PTHREAD_RWLOCK_INITIALIZER;

/* === Internal Helpers === */

static sw_mod_entry_t *mod_find_locked(const char *name) {
    for (uint32_t i = 0; i < g_module_count; i++) {
        if (g_modules[i].active && strcmp(g_modules[i].info.name, name) == 0) {
            return &g_modules[i];
        }
    }
    return NULL;
}

/* === Public API === */

sw_module_t *sw_module_register(const char *name, void (*func)(void *)) {
    if (!name || !func) return NULL;

    pthread_rwlock_wrlock(&g_module_lock);

    /* Check for existing */
    sw_mod_entry_t *existing = mod_find_locked(name);
    if (existing) {
        pthread_rwlock_unlock(&g_module_lock);
        return &existing->info;
    }

    if (g_module_count >= SW_MODULE_MAX) {
        pthread_rwlock_unlock(&g_module_lock);
        return NULL;
    }

    sw_mod_entry_t *entry = &g_modules[g_module_count++];
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->info.name, name, SW_MODULE_NAME_MAX - 1);
    entry->info.current_func = func;
    entry->info.previous_func = NULL;
    entry->info.version = 1;
    entry->active = 1;

    pthread_rwlock_unlock(&g_module_lock);
    return &entry->info;
}

sw_module_t *sw_module_find(const char *name) {
    if (!name) return NULL;

    pthread_rwlock_rdlock(&g_module_lock);
    sw_mod_entry_t *e = mod_find_locked(name);
    sw_module_t *result = e ? &e->info : NULL;
    pthread_rwlock_unlock(&g_module_lock);
    return result;
}

int sw_module_upgrade(const char *name, void (*new_func)(void *)) {
    if (!name || !new_func) return -1;

    pthread_rwlock_wrlock(&g_module_lock);

    sw_mod_entry_t *entry = mod_find_locked(name);
    if (!entry) {
        pthread_rwlock_unlock(&g_module_lock);
        return -1;
    }

    uint32_t old_version = entry->info.version;
    entry->info.previous_func = entry->info.current_func;
    entry->info.current_func = new_func;
    entry->info.version++;

    /* Notify all tracked processes */
    sw_mod_proc_t *mp = entry->procs;
    while (mp) {
        if (mp->proc && mp->proc->state != SW_PROC_FREE &&
            mp->proc->state != SW_PROC_EXITING) {
            sw_code_change_t *msg = (sw_code_change_t *)malloc(sizeof(sw_code_change_t));
            msg->old_version = old_version;
            msg->new_version = entry->info.version;
            msg->new_func = new_func;
            strncpy(msg->module_name, name, SW_MODULE_NAME_MAX - 1);
            sw_send_tagged(mp->proc, SW_TAG_CODE_CHANGE, msg);
        }
        mp = mp->next;
    }

    pthread_rwlock_unlock(&g_module_lock);
    return 0;
}

int sw_module_rollback(const char *name) {
    if (!name) return -1;

    pthread_rwlock_wrlock(&g_module_lock);

    sw_mod_entry_t *entry = mod_find_locked(name);
    if (!entry || !entry->info.previous_func) {
        pthread_rwlock_unlock(&g_module_lock);
        return -1;
    }

    void (*old) (void *) = entry->info.current_func;
    entry->info.current_func = entry->info.previous_func;
    entry->info.previous_func = old;
    entry->info.version++;

    /* Notify all tracked processes of rollback */
    sw_mod_proc_t *mp = entry->procs;
    while (mp) {
        if (mp->proc && mp->proc->state != SW_PROC_FREE) {
            sw_code_change_t *msg = (sw_code_change_t *)malloc(sizeof(sw_code_change_t));
            msg->old_version = entry->info.version - 1;
            msg->new_version = entry->info.version;
            msg->new_func = entry->info.current_func;
            strncpy(msg->module_name, name, SW_MODULE_NAME_MAX - 1);
            sw_send_tagged(mp->proc, SW_TAG_CODE_CHANGE, msg);
        }
        mp = mp->next;
    }

    pthread_rwlock_unlock(&g_module_lock);
    return 0;
}

uint32_t sw_module_version(const char *name) {
    sw_module_t *m = sw_module_find(name);
    return m ? m->version : 0;
}

sw_process_t *sw_module_spawn(const char *module_name, void *arg) {
    pthread_rwlock_rdlock(&g_module_lock);
    sw_mod_entry_t *entry = mod_find_locked(module_name);
    if (!entry) {
        pthread_rwlock_unlock(&g_module_lock);
        return NULL;
    }
    void (*func)(void *) = entry->info.current_func;
    pthread_rwlock_unlock(&g_module_lock);

    /* spawn_link + unlink to force different scheduler */
    sw_process_t *proc = sw_spawn_link(func, arg);
    if (!proc) return NULL;
    if (sw_self()) sw_unlink(proc);

    /* Track this process under the module */
    pthread_rwlock_wrlock(&g_module_lock);
    entry = mod_find_locked(module_name);
    if (entry) {
        sw_mod_proc_t *mp = (sw_mod_proc_t *)malloc(sizeof(sw_mod_proc_t));
        mp->proc = proc;
        mp->next = entry->procs;
        entry->procs = mp;
        entry->proc_count++;
    }
    pthread_rwlock_unlock(&g_module_lock);

    return proc;
}

sw_process_t *sw_module_spawn_link(const char *module_name, void *arg) {
    pthread_rwlock_rdlock(&g_module_lock);
    sw_mod_entry_t *entry = mod_find_locked(module_name);
    if (!entry) {
        pthread_rwlock_unlock(&g_module_lock);
        return NULL;
    }
    void (*func)(void *) = entry->info.current_func;
    pthread_rwlock_unlock(&g_module_lock);

    sw_process_t *proc = sw_spawn_link(func, arg);
    if (!proc) return NULL;

    /* Track */
    pthread_rwlock_wrlock(&g_module_lock);
    entry = mod_find_locked(module_name);
    if (entry) {
        sw_mod_proc_t *mp = (sw_mod_proc_t *)malloc(sizeof(sw_mod_proc_t));
        mp->proc = proc;
        mp->next = entry->procs;
        entry->procs = mp;
        entry->proc_count++;
    }
    pthread_rwlock_unlock(&g_module_lock);

    return proc;
}

void sw_module_cleanup_proc(sw_process_t *proc) {
    if (!proc) return;

    pthread_rwlock_wrlock(&g_module_lock);

    for (uint32_t i = 0; i < g_module_count; i++) {
        if (!g_modules[i].active) continue;

        sw_mod_proc_t **pp = &g_modules[i].procs;
        while (*pp) {
            if ((*pp)->proc == proc) {
                sw_mod_proc_t *found = *pp;
                *pp = found->next;
                free(found);
                g_modules[i].proc_count--;
            } else {
                pp = &(*pp)->next;
            }
        }
    }

    pthread_rwlock_unlock(&g_module_lock);
}
