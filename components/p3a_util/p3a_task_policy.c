// SPDX-License-Identifier: Apache-2.0
#include "p3a_task_policy.h"

#define PRIORITY_MIN 1
#define PRIORITY_MAX 24  // configMAX_PRIORITIES is 25; leave the top to the system

static const p3a_task_policy_t s_policy[P3A_TASK_CLASS_COUNT] = {
    [P3A_TASK_RENDER] = {.priority = 5, .core = 1, .psram_stack = false},
    [P3A_TASK_TOUCH] = {.priority = 5, .core = P3A_TASK_CORE_ANY, .psram_stack = true},
    [P3A_TASK_APP] = {.priority = 4, .core = 0, .psram_stack = true},
    [P3A_TASK_NETWORK] = {.priority = 3, .core = 0, .psram_stack = true},
    [P3A_TASK_BACKGROUND] = {.priority = 2, .core = 0, .psram_stack = true},
};

static const char *const s_names[P3A_TASK_CLASS_COUNT] = {
    [P3A_TASK_RENDER] = "render",   [P3A_TASK_TOUCH] = "touch",           [P3A_TASK_APP] = "app",
    [P3A_TASK_NETWORK] = "network", [P3A_TASK_BACKGROUND] = "background",
};

static bool class_ok(p3a_task_class_t cls)
{
    return (int)cls >= 0 && cls < P3A_TASK_CLASS_COUNT;
}

p3a_task_policy_t p3a_task_policy(p3a_task_class_t cls)
{
    return s_policy[class_ok(cls) ? cls : P3A_TASK_BACKGROUND];
}

uint8_t p3a_task_priority(p3a_task_class_t cls, int offset)
{
    int priority = (int)p3a_task_policy(cls).priority + offset;
    if (priority < PRIORITY_MIN) {
        priority = PRIORITY_MIN;
    }
    if (priority > PRIORITY_MAX) {
        priority = PRIORITY_MAX;
    }
    return (uint8_t)priority;
}

const char *p3a_task_class_name(p3a_task_class_t cls)
{
    return class_ok(cls) ? s_names[cls] : "invalid";
}
