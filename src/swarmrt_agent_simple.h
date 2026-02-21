/* SwarmRT Agent - Simple version for testing */
#ifndef SW_AGENT_H
#define SW_AGENT_H

#include "swarmrt_v2.h"
#include <stdint.h>

typedef enum { SW_TOOL_SYNC, SW_TOOL_ASYNC } sw_tool_mode_t;

typedef struct {
    char name[64];
    int (*handler)(void* proc, void* args);
    sw_tool_mode_t mode;
} sw_tool_t;

typedef struct {
    uint32_t capabilities;
    uint32_t max_tokens;
} sw_agent_ctx_t;

int sw_tool_register(const char *name, int (*handler)(void*, void*), sw_tool_mode_t mode, uint32_t timeout_ms);
int tool_read_file(void *proc, void *args);
int tool_llm_prompt(void *proc, void *args);
sw_agent_ctx_t *sw_agent_create(uint32_t capabilities);
void sw_agent_init(void);
void test_agent_workflow(void);

#endif