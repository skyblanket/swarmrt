/*
 * SwarmRT Agent Core - Async Tool Calling for AI Agents
 * 
 * This is THE key feature for running agents like me:
 * - Async tool calls (don't block schedulers)
 * - Process suspension during tool execution
 * - Automatic resumption when tool completes
 * - Context preservation across suspension
 */

#ifndef SWARMRT_AGENT_H
#define SWARMRT_AGENT_H

#include "swarmrt_full.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* === Tool System === */

#define SW_MAX_TOOLS        256
#define SW_MAX_TOOL_ARGS    16
#define SW_TOOL_NAME_LEN    64

typedef enum {
    SW_TOOL_SYNC,       /* Blocking call */
    SW_TOOL_ASYNC,      /* Non-blocking, callback */
    SW_TOOL_DEFERRED    /* Queue for batch execution */
} sw_tool_mode_t;

typedef struct {
    char name[SW_TOOL_NAME_LEN];
    sw_term_t (*handler)(sw_process_t *proc, sw_term_t args);
    sw_tool_mode_t mode;
    uint32_t timeout_ms;
    bool requires_capability;
    uint32_t capability_flag;
} sw_tool_t;

typedef struct sw_tool_call {
    uint64_t id;
    uint64_t caller_pid;
    char tool_name[SW_TOOL_NAME_LEN];
    sw_term_t args;
    sw_term_t result;
    int status;         /* 0=pending, 1=running, 2=complete, -1=error */
    uint64_t start_time;
    uint64_t end_time;
    struct sw_tool_call *next;
} sw_tool_call_t;

/* === Async Operation Structure === */

typedef struct {
    uint64_t op_id;
    sw_process_t *proc;
    void (*callback)(sw_process_t *proc, sw_term_t result);
    sw_term_t result;
    pthread_mutex_t lock;
    pthread_cond_t done;
    bool completed;
} sw_async_op_t;

/* === Agent Context === */

typedef struct {
    /* LLM conversation context */
    sw_term_t conversation_history;
    uint32_t max_tokens;
    uint32_t context_window;
    
    /* Tool capabilities */
    uint32_t capabilities;
    sw_tool_t *available_tools[SW_MAX_TOOLS];
    uint32_t num_tools;
    
    /* Pending operations */
    sw_tool_call_t *pending_calls;
    uint32_t pending_count;
    
    /* Suspension state */
    uint8_t *saved_pc;
    sw_term_t *saved_regs;
    int saved_arity;
    
    /* Stats */
    uint64_t total_calls;
    uint64_t total_tokens;
    uint64_t total_latency_ms;
} sw_agent_ctx_t;

/* === Tool Registry === */

int sw_tool_register(const char *name, 
                     sw_term_t (*handler)(sw_process_t*, sw_term_t),
                     sw_tool_mode_t mode,
                     uint32_t timeout_ms);

sw_term_t sw_tool_call(sw_process_t *proc, const char *tool_name, sw_term_t args);
sw_term_t sw_tool_call_async(sw_process_t *proc, const char *tool_name, sw_term_t args);

/* === Async Operations (The Magic) === */

/* 
 * Call an async operation and SUSPEND the process.
 * The process will be resumed when the operation completes.
 * This is how agents call tools without blocking schedulers.
 */
/* Simplified await for end-to-end testing */
int sw_await_simple(sw_process_t *proc, void (*async_fn)(void*), void *arg, sw_term_t *result);

/* Resume a suspended process with result */
void sw_resume(sw_process_t *proc, sw_term_t result);

/* === Agent Lifecycle === */

sw_agent_ctx_t *sw_agent_create(uint32_t capabilities);
void sw_agent_destroy(sw_agent_ctx_t *ctx);
int sw_agent_attach(sw_process_t *proc, sw_agent_ctx_t *ctx);

/* === Built-in Tools === */

/* File operations */
sw_term_t tool_read_file(sw_process_t *proc, sw_term_t args);
sw_term_t tool_write_file(sw_process_t *proc, sw_term_t args);
sw_term_t tool_list_dir(sw_process_t *proc, sw_term_t args);

/* Search */
sw_term_t tool_grep(sw_process_t *proc, sw_term_t args);
sw_term_t tool_web_search(sw_process_t *proc, sw_term_t args);

/* Execution */
sw_term_t tool_execute(sw_process_t *proc, sw_term_t args);
sw_term_t tool_spawn_task(sw_process_t *proc, sw_term_t args);

/* LLM */
sw_term_t tool_llm_prompt(sw_process_t *proc, sw_term_t args);
sw_term_t tool_embed(sw_process_t *proc, sw_term_t args);

/* === Swarm Coordination === */

typedef struct {
    uint64_t task_id;
    sw_term_t description;
    uint64_t assignee;
    uint64_t parent_task;
    sw_term_t dependencies;
    sw_term_t result;
    int status;
} sw_task_t;

int sw_swarm_delegate(sw_process_t *agent, sw_term_t task, uint64_t *task_id);
sw_term_t sw_swarm_collect(sw_process_t *agent, uint64_t task_id);

/* === Example Agent Process === */

/* 
 * This is what an agent process looks like:
 * 
 * void agent_main(void *arg) {
 *     sw_agent_ctx_t *ctx = sw_agent_create(CAP_FILE | CAP_WEB);
 *     sw_agent_attach(sw_current(), ctx);
 *     
 *     while (1) {
 *         sw_term_t msg = sw_receive();
 *         sw_term_t result = sw_tool_call(sw_current(), "llm_prompt", msg);
 *         
 *         // Tool call suspends process, resumes when LLM responds
 *         // Other processes run during the wait
 *         
 *         sw_send(parent, result);
 *     }
 * }
 */

#endif