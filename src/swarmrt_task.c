/*
 * SwarmRT Task - Parallel Fan-Out (async/await)
 *
 * Uses spawn_link + monitor for crash detection.
 * Child sends result tagged with SW_TAG_TASK_RESULT + monitor ref.
 * Parent awaits by scanning mailbox for matching result or DOWN.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include "swarmrt_task.h"

/* === Child Trampoline === */

typedef struct {
    void *(*func)(void *);
    void *arg;
    sw_process_t *parent;
    volatile uint64_t monitor_ref;
    _Atomic int ready; /* Set by parent after monitor_ref is written */
} sw_task_child_arg_t;

/* Result message sent from child to parent */
typedef struct {
    void *value;
    uint64_t ref;
} sw_task_result_msg_t;

static void task_child_entry(void *raw_arg) {
    sw_task_child_arg_t *ta = (sw_task_child_arg_t *)raw_arg;

    /* Wait for parent to set monitor_ref — with multi-scheduler,
     * child can start on a different thread before parent finishes setup */
    while (!atomic_load(&ta->ready)) {
#ifdef __aarch64__
        __asm__ volatile("yield");
#else
        __asm__ volatile("pause");
#endif
    }

    void *(*func)(void *) = ta->func;
    void *arg = ta->arg;
    sw_process_t *parent = ta->parent;
    uint64_t ref = ta->monitor_ref;
    free(ta);

    /* Run the user function */
    void *result = func(arg);

    /* If the function signaled an error (set exit_reason), don't send result.
     * Let the DOWN message from the monitor propagate the crash to parent. */
    if (sw_self()->exit_reason != 0) {
        return;
    }

    /* Send result back to parent tagged with our monitor ref */
    sw_task_result_msg_t *msg = (sw_task_result_msg_t *)malloc(sizeof(sw_task_result_msg_t));
    msg->value = result;
    msg->ref = ref;

    sw_send_tagged(parent, SW_TAG_TASK_RESULT, msg);
}

/* === Public API === */

sw_task_t sw_task_async(void *(*func)(void *), void *arg) {
    sw_task_t task = { .child = NULL, .monitor_ref = 0, .completed = 0 };

    sw_process_t *self = sw_self();
    if (!self) return task;

    /* Allocate child arg — child will free it */
    sw_task_child_arg_t *ta = (sw_task_child_arg_t *)malloc(sizeof(sw_task_child_arg_t));
    ta->func = func;
    ta->arg = arg;
    ta->parent = self;
    ta->monitor_ref = 0;
    atomic_store(&ta->ready, 0);

    /* Spawn linked child */
    sw_process_t *child = sw_spawn_link(task_child_entry, ta);
    if (!child) {
        free(ta);
        return task;
    }

    /* Monitor the child */
    uint64_t ref = sw_monitor(child);

    /* Set the ref and signal the child it's safe to proceed.
     * With multi-scheduler, child may already be running on another thread. */
    ta->monitor_ref = ref;
    atomic_store_explicit(&ta->ready, 1, memory_order_release);

    task.child = child;
    task.monitor_ref = ref;
    return task;
}

sw_task_result_t sw_task_await(sw_task_t *task, uint64_t timeout_ms) {
    sw_task_result_t result = { .status = SW_TASK_TIMEOUT, .value = NULL };

    if (!task || !task->child || task->completed) {
        result.status = SW_TASK_CRASH;
        return result;
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        /* Calculate remaining time */
        uint64_t remaining = timeout_ms;
        if (timeout_ms != (uint64_t)-1 && timeout_ms > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                              (now.tv_nsec - start.tv_nsec) / 1000000;
            if (elapsed >= timeout_ms) {
                result.status = SW_TASK_TIMEOUT;
                return result;
            }
            remaining = timeout_ms - elapsed;
        }

        uint64_t tag = 0;
        void *msg = sw_receive_any(remaining, &tag);

        if (!msg && tag == 0) {
            /* Timeout or killed */
            result.status = SW_TASK_TIMEOUT;
            return result;
        }

        if (tag == SW_TAG_TASK_RESULT) {
            sw_task_result_msg_t *rmsg = (sw_task_result_msg_t *)msg;
            if (rmsg->ref == task->monitor_ref) {
                /* Our task completed */
                result.status = SW_TASK_OK;
                result.value = rmsg->value;
                task->completed = 1;
                free(rmsg);

                /* Demonitor — no longer need DOWN messages */
                sw_demonitor(task->monitor_ref);

                /* Unlink from child (it's about to exit normally) */
                if (task->child) {
                    sw_unlink(task->child);
                }

                return result;
            }
            /* Not our task — re-enqueue */
            sw_send_tagged(sw_self(), SW_TAG_TASK_RESULT, msg);
            continue;
        }

        if (tag == SW_TAG_DOWN) {
            sw_signal_t *sig = (sw_signal_t *)msg;
            if (sig->ref == task->monitor_ref) {
                if (sig->reason != 0) {
                    /* Our task crashed (abnormal exit) */
                    result.status = SW_TASK_CRASH;
                    result.value = NULL;
                    task->completed = 1;
                    free(sig);
                    return result;
                }
                /* Normal exit DOWN — task sent result before exiting.
                 * Discard this DOWN and keep looking for the TASK_RESULT. */
                free(sig);
                continue;
            }
            /* Not our task's DOWN — re-enqueue */
            sw_send_tagged(sw_self(), SW_TAG_DOWN, msg);
            continue;
        }

        /* Other message — re-enqueue */
        sw_send_tagged(sw_self(), tag, msg);

        /* For non-blocking yield (timeout=0), only scan once */
        if (timeout_ms == 0) {
            result.status = SW_TASK_TIMEOUT;
            return result;
        }
    }
}

sw_task_result_t sw_task_yield(sw_task_t *task) {
    return sw_task_await(task, 0);
}

void sw_task_shutdown(sw_task_t *task) {
    if (!task || !task->child || task->completed) return;

    /* Demonitor to avoid receiving DOWN */
    sw_demonitor(task->monitor_ref);

    /* Unlink to avoid exit signal propagation */
    sw_unlink(task->child);

    /* Kill the child (lock-free) */
    sw_process_kill(task->child, -1);

    task->completed = 1;
}
