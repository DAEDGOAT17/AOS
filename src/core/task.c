#include "task.h"
#include "kmalloc.h"
#include "screen.h"
#include "timer.h"


static task_t tasks[MAX_TASKS];
static uint32_t next_task_id = 0;
static uint32_t current_task_id = 0;

void task_init() {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_DEAD;
        tasks[i].id = 0;
    }
    
    // Create idle task (ID 0)
    tasks[0].id = next_task_id++;
    tasks[0].state = TASK_READY;
    tasks[0].priority = 0;
    tasks[0].ticks_used = 0;
    for (int i = 0; i < 32; i++) tasks[0].name[i] = 0;
    tasks[0].name[0] = 'i'; tasks[0].name[1] = 'd'; 
    tasks[0].name[2] = 'l'; tasks[0].name[3] = 'e';
}

int task_create(const char* name, void (*function)(void), uint32_t priority) {
    if (!name || !function) return -1;

    // Find free slot
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_DEAD) {
            tasks[i].id = next_task_id++;
            tasks[i].state = TASK_READY;
            tasks[i].priority = priority;
            tasks[i].function = function;
            tasks[i].ticks_used = 0;
            tasks[i].wake_time = 0;
            
            // Copy name
            int j;
            for (j = 0; j < 31 && name[j]; j++) {
                tasks[i].name[j] = name[j];
            }
            tasks[i].name[j] = '\0';
            
            return tasks[i].id;
        }
    }
    return -1;  // No free slots
}

void task_kill(uint32_t id) {
    if (id == 0) return;

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].id == id) {
            tasks[i].state = TASK_DEAD;
            return;
        }
    }
}

task_t* task_get(uint32_t id) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].id == id && tasks[i].state != TASK_DEAD) {
            return &tasks[i];
        }
    }
    return NULL;
}

void task_list() {
    print_string("PID  State    Name\n");
    print_string("---  -------  ----------------\n");
    
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_DEAD) {
            kprint_dec(tasks[i].id);
            print_string("  ");
            
            switch (tasks[i].state) {
                case TASK_READY:   print_string("READY   "); break;
                case TASK_RUNNING: print_string("RUNNING "); break;
                case TASK_WAITING: print_string("WAITING "); break;
            }
            
            print_string(" ");
            print_string(tasks[i].name);
            print_char('\n');
        }
    }
}

void task_yield() {
    uint32_t now = timer_get_ticks();
    for (uint32_t index = 1; index < MAX_TASKS; index++) {
        if (tasks[index].state == TASK_WAITING &&
            (int32_t)(now - tasks[index].wake_time) >= 0) {
            tasks[index].wake_time = 0;
            tasks[index].state = TASK_READY;
        }
    }

    for (uint32_t offset = 1; offset <= MAX_TASKS; offset++) {
        uint32_t next = (current_task_id + offset) % MAX_TASKS;
        if (tasks[next].state != TASK_READY) continue;

        current_task_id = next;
        if (!tasks[next].function) return;

        tasks[next].state = TASK_RUNNING;
        tasks[next].function();
        if (tasks[next].state == TASK_RUNNING)
            tasks[next].state = TASK_READY;
        return;
    }
}

void task_sleep(uint32_t ticks) {
    if (current_task_id == 0 || current_task_id >= MAX_TASKS) return;
    task_t* current = &tasks[current_task_id];
    if (current->state != TASK_RUNNING) return;
    current->wake_time = timer_get_ticks() + ticks;
    current->state = TASK_WAITING;
}