#ifndef INFER_H
#define INFER_H

#include <stddef.h>
#include <stdint.h>

#define INFER_REASON_LEN 128

typedef enum {
    INFER_ACTION_NONE = 0,
    INFER_ACTION_HELP,
    INFER_ACTION_SHOW_STATUS,
    INFER_ACTION_LIST_FILES,
    INFER_ACTION_CLEAR_SCREEN,
    INFER_ACTION_REBOOT,
    INFER_ACTION_REVIEW_PATCH,
    INFER_ACTION_ALERT_OPERATOR,
    INFER_ACTION_STOP_TASK,
    INFER_ACTION_PATCH_CONFIG,
    INFER_ACTION_DISPATCH_AGENT,
    INFER_ACTION_COUNT
} infer_action_t;

typedef struct {
    infer_action_t action;
    float confidence;
    int safe;
    char reason[INFER_REASON_LEN];
} infer_result_t;

int infer_decide(const char* prompt, infer_result_t* result);
const char* infer_action_name(infer_action_t action);
int infer_route_action(const char* prompt, char* command, size_t capacity);
int infer_route_decision(const infer_result_t* result, char* command, size_t capacity);
int infer_execute_decision(const infer_result_t* result);
int infer_execute_prompt(const char* prompt, infer_result_t* result);

#endif
