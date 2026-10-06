#ifndef AGENT_H
#define AGENT_H

#include <stdint.h>

// Initialize the agentic subsystem (creates the /agent/db directories on FAT32)
void agent_init(void);

// Set a context variable in the agent database
void agent_ctx_set(const char* key, const char* value);

// Get a context variable from the agent database
void agent_ctx_get(const char* key);

// Start an autonomous agent task
void agent_task(const char* instruction);

// Persist the latest AI result in the agent database
void agent_task_result(const char* result);

// Persist an action and its observed output without completing the task
void agent_task_observation(const char* action, const char* output);

// Record that the bounded agent loop requires review
void agent_task_failed(const char* reason);

// Persist a bounded safety-checked execution plan in the agent database
void agent_plan(const char* instruction);

// Complete the latest task by reading its context and writing a short result
void agent_complete_task(void);

// Evaluate the latest task state and set a safe status: complete or needs_review
void agent_selfcheck(void);

#endif // AGENT_H
