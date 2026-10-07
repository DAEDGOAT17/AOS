#include "infer.h"
#include "agent.h"
#include "string.h"

void shell_execute(char* cmd);

static int infer_is_phrase(const char* text, const char* phrase) {
    if (!text || !phrase) return 0;
    return strstr(text, phrase) != NULL;
}

static void infer_lowercase(char* output, size_t capacity, const char* input) {
    size_t index = 0;
    if (capacity == 0) return;
    while (input && *input && index + 1 < capacity) {
        char ch = *input++;
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-') {
            output[index++] = ch;
        } else if (ch == '_' || ch == '/' || ch == '.') {
            output[index++] = ' ';
        }
    }
    output[index] = '\0';
}

int infer_decide(const char* prompt, infer_result_t* result) {
    if (!result) return -1;
    memset(result, 0, sizeof(*result));
    result->action = INFER_ACTION_NONE;
    result->confidence = 0.0f;
    result->safe = 1;
    strcpy(result->reason, "no command matched");

    if (!prompt || prompt[0] == '\0') {
        strcpy(result->reason, "empty prompt");
        return 0;
    }

    char normalized[256];
    infer_lowercase(normalized, sizeof(normalized), prompt);

    if (infer_is_phrase(normalized, "help") || infer_is_phrase(normalized, "what can you do") || infer_is_phrase(normalized, "show help") || infer_is_phrase(normalized, "list commands") || infer_is_phrase(normalized, "show the commands")) {
        result->action = INFER_ACTION_HELP;
        result->confidence = 0.94f;
        result->safe = 1;
        strcpy(result->reason, "help request matched the safe shell help action");
        return 0;
    }

    if (infer_is_phrase(normalized, "reboot") || infer_is_phrase(normalized, "restart the system") || infer_is_phrase(normalized, "restart")) {
        result->action = INFER_ACTION_REBOOT;
        result->confidence = 0.91f;
        result->safe = 1;
        strcpy(result->reason, "reboot request matched safe system control policy");
        return 0;
    }

    if (infer_is_phrase(normalized, "status") || infer_is_phrase(normalized, "sysinfo") || infer_is_phrase(normalized, "health") || infer_is_phrase(normalized, "system status") || infer_is_phrase(normalized, "state of the machine") || infer_is_phrase(normalized, "tell me about the state of the machine") || infer_is_phrase(normalized, "show me the state of the machine") || infer_is_phrase(normalized, "state of machine")) {
        result->action = INFER_ACTION_SHOW_STATUS;
        result->confidence = 0.89f;
        result->safe = 1;
        strcpy(result->reason, "status query matched the safe local telemetry action");
        return 0;
    }

    if ((infer_is_phrase(normalized, "list") && (infer_is_phrase(normalized, "file") || infer_is_phrase(normalized, "directory"))) || infer_is_phrase(normalized, "ls ")) {
        result->action = INFER_ACTION_LIST_FILES;
        result->confidence = 0.87f;
        result->safe = 1;
        strcpy(result->reason, "directory listing intent matched the local shell action");
        return 0;
    }

    if ((infer_is_phrase(normalized, "clear") && (infer_is_phrase(normalized, "screen") || infer_is_phrase(normalized, "terminal"))) || infer_is_phrase(normalized, "clear screen")) {
        result->action = INFER_ACTION_CLEAR_SCREEN;
        result->confidence = 0.85f;
        result->safe = 1;
        strcpy(result->reason, "clear-screen intent matched the safe UI command");
        return 0;
    }

    if (infer_is_phrase(normalized, "stop") || infer_is_phrase(normalized, "halt") || infer_is_phrase(normalized, "shutdown")) {
        result->action = INFER_ACTION_STOP_TASK;
        result->confidence = 0.83f;
        result->safe = 1;
        strcpy(result->reason, "stop or halt intent matched the bounded task control action");
        return 0;
    }

    if (infer_is_phrase(normalized, "patch") || infer_is_phrase(normalized, "improve") || infer_is_phrase(normalized, "upgrade")) {
        result->action = INFER_ACTION_PATCH_CONFIG;
        result->confidence = 0.78f;
        result->safe = 1;
        strcpy(result->reason, "bounded improvement intent matched the self-upgrade policy");
        return 0;
    }

    if (infer_is_phrase(normalized, "plan") || infer_is_phrase(normalized, "research") ||
        infer_is_phrase(normalized, "analyze") || infer_is_phrase(normalized, "build") ||
        infer_is_phrase(normalized, "write") || infer_is_phrase(normalized, "explain") ||
        infer_is_phrase(normalized, "summarize") || infer_is_phrase(normalized, "compare")) {
        result->action = INFER_ACTION_DISPATCH_AGENT;
        result->confidence = 0.80f;
        result->safe = 1;
        strcpy(result->reason, "open-ended request routed to the bounded agent runtime");
        return 0;
    }

    result->action = INFER_ACTION_NONE;
    result->confidence = 0.17f;
    result->safe = 1;
    strcpy(result->reason, "no safe action matched; prompt remains local and non-destructive");
    return 0;
}

int infer_route_decision(const infer_result_t* result, char* command, size_t capacity) {
    if (!result || !command || capacity == 0) return -1;
    command[0] = '\0';

    switch (result->action) {
        case INFER_ACTION_HELP:
            strncpy(command, "help", capacity - 1);
            break;
        case INFER_ACTION_SHOW_STATUS:
            strncpy(command, "sysinfo", capacity - 1);
            break;
        case INFER_ACTION_LIST_FILES:
            strncpy(command, "ls /", capacity - 1);
            break;
        case INFER_ACTION_CLEAR_SCREEN:
            strncpy(command, "clear", capacity - 1);
            break;
        case INFER_ACTION_REBOOT:
            strncpy(command, "reboot", capacity - 1);
            break;
        case INFER_ACTION_STOP_TASK:
            strncpy(command, "agent_selfcheck", capacity - 1);
            break;
        case INFER_ACTION_PATCH_CONFIG:
            strncpy(command, "agent_plan improve", capacity - 1);
            break;
        case INFER_ACTION_ALERT_OPERATOR:
            strncpy(command, "agent_ctx_set alert status", capacity - 1);
            break;
        case INFER_ACTION_REVIEW_PATCH:
            strncpy(command, "agent_selfcheck", capacity - 1);
            break;
        case INFER_ACTION_DISPATCH_AGENT:
            strncpy(command, "echo agent dispatch requires original prompt", capacity - 1);
            break;
        case INFER_ACTION_NONE:
        default:
            strncpy(command, "echo no safe action matched", capacity - 1);
            break;
    }

    command[capacity - 1] = '\0';
    return 0;
}

int infer_route_action(const char* prompt, char* command, size_t capacity) {
    infer_result_t result;
    if (infer_decide(prompt, &result) != 0) return -1;
    return infer_route_decision(&result, command, capacity);
}

int infer_execute_decision(const infer_result_t* result) {
    char command[128];
    if (!result) return -1;
    if (infer_route_decision(result, command, sizeof(command)) != 0) return -1;
    if (command[0] == '\0' || !result->safe) {
        shell_execute("echo no safe action matched");
        return 0;
    }
    shell_execute(command);
    return 0;
}

int infer_execute_prompt(const char* prompt, infer_result_t* result) {
    if (!prompt || !result) return -1;
    if (infer_decide(prompt, result) != 0) return -1;
    if (!result->safe) return infer_execute_decision(result);
    if (result->action == INFER_ACTION_DISPATCH_AGENT) {
        agent_task(prompt);
        return 0;
    }
    return infer_execute_decision(result);
}

const char* infer_action_name(infer_action_t action) {
    switch (action) {
        case INFER_ACTION_NONE: return "none";
        case INFER_ACTION_HELP: return "help";
        case INFER_ACTION_SHOW_STATUS: return "show_status";
        case INFER_ACTION_LIST_FILES: return "list_files";
        case INFER_ACTION_CLEAR_SCREEN: return "clear_screen";
        case INFER_ACTION_REBOOT: return "reboot";
        case INFER_ACTION_REVIEW_PATCH: return "review_patch";
        case INFER_ACTION_ALERT_OPERATOR: return "alert_operator";
        case INFER_ACTION_STOP_TASK: return "stop_task";
        case INFER_ACTION_PATCH_CONFIG: return "patch_config";
        case INFER_ACTION_DISPATCH_AGENT: return "dispatch_agent";
        default: return "unknown";
    }
}
