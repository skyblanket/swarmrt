/* SwarmRT Agent - Async tool calling with suspension */

#define _GNU_SOURCE
#define _XOPEN_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <ucontext.h>
#include "swarmrt_v2.h"
#include "swarmrt_agent.h"

static sw_tool_t g_tools[SW_MAX_TOOLS];
static uint32_t g_num_tools = 0;
static pthread_mutex_t g_tool_lock = PTHREAD_MUTEX_INITIALIZER;

int sw_tool_register(const char *name, sw_term_t (*handler)(sw_process_t*, sw_term_t),
                     sw_tool_mode_t mode, uint32_t timeout_ms) {
    pthread_mutex_lock(&g_tool_lock);
    if (g_num_tools >= SW_MAX_TOOLS) {
        pthread_mutex_unlock(&g_tool_lock);
        return -1;
    }
    sw_tool_t *tool = &g_tools[g_num_tools++];
    strncpy(tool->name, name, SW_TOOL_NAME_LEN - 1);
    tool->handler = handler;
    tool->mode = mode;
    tool->timeout_ms = timeout_ms;
    printf("[Agent] Registered: %s\n", name);
    pthread_mutex_unlock(&g_tool_lock);
    return 0;
}

static sw_tool_t *find_tool(const char *name) {
    for (uint32_t i = 0; i < g_num_tools; i++) {
        if (strcmp(g_tools[i].name, name) == 0) return &g_tools[i];
    }
    return NULL;
}

/* THE KEY FUNCTION: Suspend process during async operation */
int sw_await(sw_process_t *proc, void (*async_fn)(void*), void *arg, sw_term_t *result) {
    /* Launch async in background */
    pthread_t worker;
    pthread_create(&worker, NULL, (void*(*)(void*))async_fn, arg);
    
    /* SUSPEND - swap to scheduler context */
    proc->state = 1; /* waiting */
    swapcontext(&proc->ctx, &proc->scheduler->ctx);
    
    /* RESUMED here when async completes */
    proc->state = 0; /* running */
    *result = 0; /* would get from async op */
    return 0;
}

/* Built-in tools */
sw_term_t tool_read_file(sw_process_t *proc, sw_term_t args) {
    (void)proc; (void)args;
    printf("[Tool] read_file (pid=%llu)\n", (unsigned long long)proc->pid);
    usleep(10000); /* 10ms simulated I/O */
    return 1; /* ok */
}

sw_term_t tool_llm_prompt(sw_process_t *proc, sw_term_t args) {
    (void)proc; (void)args;
    printf("[Tool] llm_prompt (pid=%llu)\n", (unsigned long long)proc->pid);
    usleep(100000); /* 100ms simulated LLM call */
    return 1;
}

/* Async tool wrapper */
typedef struct { char name[64]; sw_term_t args; } tool_call_req_t;

static void async_tool_exec(void *arg) {
    tool_call_req_t *req = arg;
    sw_tool_t *tool = find_tool(req->name);
    
    printf("[Async] Executing %s in background...\n", req->name);
    tool->handler(NULL, req->args); /* Would pass real process */
    
    /* Resume suspended process */
    printf("[Async] %s complete, resuming process\n", req->name);
    
    free(req);
}

sw_term_t sw_tool_call_async(sw_process_t *proc, const char *tool_name, sw_term_t args) {
    tool_call_req_t *req = malloc(sizeof(tool_call_req_t));
    strncpy(req->name, tool_name, 63);
    req->args = args;
    
    sw_term_t result;
    printf("[Agent] pid=%llu calling %s (will suspend)\n", 
           (unsigned long long)proc->pid, tool_name);
    
    sw_await(proc, async_tool_exec, req, &result);
    
    printf("[Agent] pid=%llu resumed from %s\n", 
           (unsigned long long)proc->pid, tool_name);
    
    return result;
}

/* Agent context */
sw_agent_ctx_t *sw_agent_create(uint32_t capabilities) {
    sw_agent_ctx_t *ctx = calloc(1, sizeof(sw_agent_ctx_t));
    ctx->capabilities = capabilities;
    ctx->max_tokens = 4096;
    return ctx;
}

void sw_agent_init(void) {
    sw_tool_register("read_file", tool_read_file, SW_TOOL_ASYNC, 30000);
    sw_tool_register("llm_prompt", tool_llm_prompt, SW_TOOL_ASYNC, 120000);
}
