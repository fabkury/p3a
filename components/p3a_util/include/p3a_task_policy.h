// SPDX-License-Identifier: Apache-2.0
// Task classes: the one place that says what priority a task runs at, which
// core it runs on, and where its stack lives. Every p3a task belongs to a
// class; nothing passes a literal priority to FreeRTOS.
//
// Host-testable: no FreeRTOS or IDF includes. p3a_task.h creates the tasks.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    // The display pipeline: producer, consumer, upscale workers. Core 1 is
    // theirs (the jitter work moved everything else off it). Internal-RAM
    // stacks, because these tasks must keep running while the flash cache is
    // disabled.
    P3A_TASK_RENDER,
    // Touch input and gesture handling.
    P3A_TASK_TOUCH,
    // The player's own logic: scheduler, loader, job runners, event dispatch.
    P3A_TASK_APP,
    // Anything that talks to the network: Wi-Fi management, HTTP handlers'
    // helpers, MQTT, downloads, refresh.
    P3A_TASK_NETWORK,
    // Housekeeping that must never get in the way: eviction, reporters,
    // test hooks.
    P3A_TASK_BACKGROUND,

    P3A_TASK_CLASS_COUNT
} p3a_task_class_t;

#define P3A_TASK_CORE_ANY (-1)

typedef struct {
    uint8_t priority;  // FreeRTOS priority, higher runs first
    int8_t core;       // 0, 1, or P3A_TASK_CORE_ANY
    bool psram_stack;  // stack allocated from PSRAM (internal RAM is the fallback)
} p3a_task_policy_t;

// The policy of a class. An out-of-range class gets BACKGROUND's.
//
// Priorities are the ones 1.x shipped with (render 5, touch 5, app 4,
// network 3), not the 8 and 6 its Kconfig defaults suggested. Networking and
// app logic stay on core 0 and the display pipeline on core 1, as the jitter
// work established (docs/jitter/REPORT.md).
p3a_task_policy_t p3a_task_policy(p3a_task_class_t cls);

// Applies a per-task offset to a class priority, clamped to 1..24 so a task
// can never sink to the idle priority or rise above the system tasks.
uint8_t p3a_task_priority(p3a_task_class_t cls, int offset);

const char *p3a_task_class_name(p3a_task_class_t cls);

#ifdef __cplusplus
}
#endif
