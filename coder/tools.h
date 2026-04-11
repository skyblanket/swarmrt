/*
 * coder/tools.h — tool handlers for the coder CLI.
 *
 * Each handler takes a JSON arguments string and returns a malloc'd result
 * string (the tool output that gets fed back into the model). On error the
 * result string explains the error; we never return NULL on a recognized
 * tool call.
 */
#ifndef CODER_TOOLS_H
#define CODER_TOOLS_H

#include "llm.h"

/* Static list of tool definitions the CLI exposes to the model. */
extern const llm_tool_def_t CODER_TOOLS[];
extern const int CODER_TOOLS_COUNT;

/* Dispatch by name. Returns a malloc'd string the caller must free.
 * If name is unknown, returns an error string (still malloc'd, non-NULL). */
char *coder_tool_exec(const char *name, const char *arguments_json);

#endif
