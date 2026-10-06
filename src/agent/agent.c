#include "agent.h"
#include "fat32.h"
#include "net_stack.h"
#include "screen.h"
#include "string.h"

static const char* agent_storage_key(const char* key) {
    if (strcmp(key, "task_last") == 0) return "task";
    if (strcmp(key, "task_plan") == 0) return "plan";
    if (strcmp(key, "task_status") == 0) return "status";
    if (strcmp(key, "task_last_result") == 0) return "result";
    if (strcmp(key, "task_result_source") == 0) return "source";
    if (strcmp(key, "task_observation") == 0) return "observe";
    return key;
}

static int agent_read_text(const char* key, char* value, uint32_t capacity) {
    if (!key || !value || capacity == 0) return -1;

    char path[128];
    strcpy(path, "/agent/db/");
    strcat(path, agent_storage_key(key));
    strcat(path, ".txt");

    int fd = fat32_open(path, 'r');
    if (fd < 0) return -1;

    int bytes = fat32_read(fd, value, capacity - 1);
    fat32_close(fd);
    if (bytes < 0) return -1;
    value[bytes] = '\0';
    return bytes;
}

// Initialize the Agent directory structure
void agent_init(void) {
    fat32_mkdir("/agent");
    fat32_mkdir("/agent/db");
}

void agent_ctx_set(const char* key, const char* value) {
    char path[128];
    strcpy(path, "/agent/db/");
    strcat(path, agent_storage_key(key));
    strcat(path, ".txt");

    int fd = fat32_open(path, 'w');
    if (fd >= 0) {
        fat32_write(fd, value, strlen(value));
        fat32_close(fd);
        set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
        print_string("Agent DB: Set '");
        print_string(key);
        print_string("' successfully.\n");
        reset_text_color();
    } else {
        set_text_color(MAKE_COLOR(COLOR_LIGHT_RED, COLOR_BLACK));
        print_string("Agent DB ERROR: Failed to write context.\n");
        reset_text_color();
    }
}

void agent_task_result(const char* result) {
    if (!result) result = "empty";
    agent_ctx_set("task_last_result", result);
    agent_ctx_set("task_status", "complete");
}

void agent_task_observation(const char* action, const char* output) {
    char observation[512];
    int index = 0;
    const char* prefix = "Action: ";
    while (*prefix && index < (int)sizeof(observation) - 1)
        observation[index++] = *prefix++;
    while (action && *action && index < (int)sizeof(observation) - 1)
        observation[index++] = *action++;
    prefix = "\nOutput:\n";
    while (*prefix && index < (int)sizeof(observation) - 1)
        observation[index++] = *prefix++;
    while (output && *output && index < (int)sizeof(observation) - 1)
        observation[index++] = *output++;
    observation[index] = '\0';
    agent_ctx_set("task_observation", observation);
}

void agent_task_failed(const char* reason) {
    if (!reason) reason = "Agent stopped without verified completion.";
    agent_ctx_set("task_last_result", reason);
    agent_ctx_set("task_status", "needs_review");
}

void agent_ctx_get(const char* key) {
    char path[128];
    strcpy(path, "/agent/db/");
    strcat(path, agent_storage_key(key));
    strcat(path, ".txt");

    int fd = fat32_open(path, 'r');
    if (fd >= 0) {
        char buf[513];
        int bytes;
        set_text_color(MAKE_COLOR(COLOR_YELLOW, COLOR_BLACK));
        print_string("Agent DB Context [");
        print_string(key);
        print_string("]:\n");
        reset_text_color();
        while ((bytes = fat32_read(fd, buf, 512)) > 0) {
            buf[bytes] = '\0';
            print_string(buf);
        }
        fat32_close(fd);
        print_string("\n");
    } else {
        set_text_color(MAKE_COLOR(COLOR_LIGHT_RED, COLOR_BLACK));
        print_string("Agent DB ERROR: Context '");
        print_string(key);
        print_string("' not found.\n");
        reset_text_color();
    }
}

void agent_plan(const char* instruction) {
    if (!instruction || instruction[0] == '\0') {
        set_text_color(MAKE_COLOR(COLOR_LIGHT_RED, COLOR_BLACK));
        print_string("Agent plan ERROR: no instruction supplied.\n");
        reset_text_color();
        return;
    }

    char plan[512];
    strcpy(plan, "1. Inspect context and state for '");
    strcat(plan, instruction);
    strcat(plan, "'.\n2. Keep the change bounded to agent metadata or file state.\n3. Validate with a readback and persist the outcome.");

    agent_ctx_set("task_last", instruction);
    agent_ctx_set("task_status", "planned");
    agent_ctx_set("task_plan", plan);
    agent_ctx_set("task_last_result", "");

    set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
    print_string("Agent plan: persisted and bounded.\n");
    reset_text_color();
}

void agent_complete_task(void) {
    char task_last[256];
    char observation[512];
    char status[32];
    char summary[512];

    int bytes_last = agent_read_text("task_last", task_last, sizeof(task_last));
    int bytes_observation = agent_read_text("task_observation", observation, sizeof(observation));
    int bytes_status = agent_read_text("task_status", status, sizeof(status));

    if (bytes_last <= 0 || bytes_observation <= 0 || bytes_status <= 0 ||
        strcmp(status, "running") != 0) {
        set_text_color(MAKE_COLOR(COLOR_LIGHT_RED, COLOR_BLACK));
        print_string("Agent completion ERROR: no running task with action evidence.\n");
        agent_task_failed("Completion refused: no running task with action evidence.");
        reset_text_color();
        return;
    }

    strcpy(summary, "Executed read-only task: ");
    strcat(summary, task_last);
    strcat(summary, ". Action output is stored in task_observation.");

    agent_task_result(summary);

    set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
    print_string("Agent completion: observed action recorded; task marked complete.\n");
    reset_text_color();
}

void agent_selfcheck(void) {
    char task_last[256];
    char task_plan[512];
    char result[512];
    char observation[512];
    char status[32];

    int has_last = agent_read_text("task_last", task_last, sizeof(task_last)) > 0;
    int has_plan = agent_read_text("task_plan", task_plan, sizeof(task_plan)) > 0;
    int has_result = agent_read_text("task_last_result", result, sizeof(result)) > 0;
    int has_observation = agent_read_text("task_observation", observation, sizeof(observation)) > 0;
    int has_status = agent_read_text("task_status", status, sizeof(status)) > 0;

    if (has_last && has_plan && has_result && has_observation && has_status &&
        strcmp(status, "complete") == 0) {
        set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
        print_string("Agent self-check: completion and action evidence verified.\n");
        reset_text_color();
        return;
    }

    agent_ctx_set("task_status", "needs_review");
    set_text_color(MAKE_COLOR(COLOR_YELLOW, COLOR_BLACK));
    print_string("Agent self-check: task needs review.\n");
    reset_text_color();
}

void agent_task(const char* instruction) {
    char ai_ip[32];
    if (!instruction || instruction[0] == '\0') {
        print_string("Usage: agent_task <goal>\n");
        return;
    }

    set_text_color(MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_string(">> Starting bounded agent task: ");
    print_string(instruction);
    print_string("\n");
    reset_text_color();

    agent_plan(instruction);
    agent_ctx_set("task_observation", "");
    agent_ctx_set("task_status", "running");

    if (agent_read_text("ai_ip", ai_ip, sizeof(ai_ip)) <= 0)
        strcpy(ai_ip, "172.16.100.1");
    ollama_agent_request(ai_ip, instruction);
}
