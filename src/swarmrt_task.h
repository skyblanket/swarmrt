/*
 * SwarmRT Task - Parallel Fan-Out (async/await)
 *
 * Thin wrapper over spawn_link + monitor + tagged messages.
 * Child runs user function, sends result back tagged with monitor ref, exits.
 * Parent blocks on receive looking for result or DOWN (crash).
 */

#ifndef SWARMRT_TASK_H
#define SWARMRT_TASK_H

#include "swarmrt_native.h"

typedef enum {
    SW_TASK_OK = 0,
    SW_TASK_CRASH,
    SW_TASK_TIMEOUT,
} sw_task_status_t;

typedef struct {
    sw_process_t *child;
    uint64_t monitor_ref;
    int completed;
} sw_task_t;

typedef struct {
    sw_task_status_t status;
    void *value;
} sw_task_result_t;

sw_task_t sw_task_async(void *(*func)(void *), void *arg);
sw_task_result_t sw_task_await(sw_task_t *task, uint64_t timeout_ms);
sw_task_result_t sw_task_yield(sw_task_t *task);
void sw_task_shutdown(sw_task_t *task);

#endif /* SWARMRT_TASK_H */
