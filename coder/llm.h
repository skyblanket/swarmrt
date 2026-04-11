/*
 * coder/llm.h — OpenAI-compatible chat completions client
 *
 * Minimal HTTP client (via curl popen) + tiny JSON builder/parser
 * for talking to a vLLM server serving Gemma 4 (or any OpenAI-compat model).
 *
 * v1: synchronous, one request at a time. Good enough for a REPL.
 */
#ifndef CODER_LLM_H
#define CODER_LLM_H

#include <stddef.h>

/* ----- Tool definition ----- */
typedef struct {
    const char *name;
    const char *description;
    /* Raw JSON Schema object string for the "parameters" field.
     * e.g. "{\"type\":\"object\",\"properties\":{...},\"required\":[...]}"
     */
    const char *params_schema_json;
} llm_tool_def_t;

/* ----- Tool call parsed from assistant response ----- */
#define LLM_MAX_TOOL_CALLS 8
typedef struct {
    char id[64];
    char name[64];
    char *arguments_json; /* malloc'd, raw JSON string of the arguments object */
} llm_tool_call_t;

/* ----- Message in conversation history ----- */
typedef struct {
    char role[16];        /* "system" | "user" | "assistant" | "tool" */
    char *content;        /* malloc'd, may be NULL (assistant tool-only messages) */
    char *tool_call_id;   /* malloc'd, only for role="tool" */
    int num_tool_calls;   /* only for role="assistant" */
    llm_tool_call_t tool_calls[LLM_MAX_TOOL_CALLS];
} llm_message_t;

/* ----- Client config ----- */
typedef struct {
    const char *endpoint; /* e.g. "http://sushi:8000" */
    const char *model;    /* e.g. "google/gemma-4-31B-it" */
    const char *api_key;  /* may be NULL */
    int verbose;          /* print request/response to stderr */
} llm_client_t;

/* Free contents of a message (content, tool_call_id, tool_calls[*].arguments_json).
 * Does NOT free the message struct itself. Safe to call on zero-inited structs. */
void llm_message_free(llm_message_t *msg);

/* Send a chat completion request.
 * - msgs/num_msgs: full conversation so far
 * - tools/num_tools: available tools (may be 0)
 * - out_assistant: filled with assistant's reply (content and/or tool_calls)
 * Returns 0 on success, -1 on error (stderr will carry the reason).
 * Caller must llm_message_free(out_assistant) when done.
 */
int llm_chat(const llm_client_t *c,
             const llm_message_t *msgs, int num_msgs,
             const llm_tool_def_t *tools, int num_tools,
             llm_message_t *out_assistant);

#endif
