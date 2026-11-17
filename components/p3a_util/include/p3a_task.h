// SPDX-License-Identifier: Apache-2.0
// The one way p3a creates a FreeRTOS task. Priority, core and stack memory
// come from the task's class (p3a_task_policy.h); call sites state only what
// is particular to them.
//
// Tasks made here are long-lived: they run for the life of the firmware and
// never return or delete themselves. Work that runs once and ends is a job
// (p3a_job.h), not a task.
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p3a_task_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P3A_TASK_CORE_CLASS (-2)  // use the class's core (the default)

typedef struct {
    const char *name;          // at most configMAX_TASK_NAME_LEN - 1 characters are kept
    TaskFunction_t fn;         // must never return
    void *arg;
    uint32_t stack_bytes;
    p3a_task_class_t cls;
    int8_t priority_offset;    // relative to the class; 0 for almost every task
    int8_t core;               // P3A_TASK_CORE_CLASS, P3A_TASK_CORE_ANY, 0 or 1
    bool internal_stack;       // force an internal-RAM stack; say why at the call site
} p3a_task_config_t;

// Creates the task. A PSRAM stack that cannot be allocated falls back to
// internal RAM with a warning. *out_handle may be NULL.
//
// Leave `core` zero-initialized only if you mean core 0: set
// P3A_TASK_CORE_CLASS explicitly, or use P3A_TASK_CONFIG().
esp_err_t p3a_task_create(const p3a_task_config_t *config, TaskHandle_t *out_handle);

// The usual case: a task that takes everything from its class.
#define P3A_TASK_CONFIG(name_, fn_, arg_, stack_bytes_, cls_) \
    ((p3a_task_config_t){.name = (name_),                      \
                         .fn = (fn_),                          \
                         .arg = (arg_),                        \
                         .stack_bytes = (stack_bytes_),        \
                         .cls = (cls_),                        \
                         .priority_offset = 0,                 \
                         .core = P3A_TASK_CORE_CLASS,          \
                         .internal_stack = false})

#ifdef __cplusplus
}
#endif
