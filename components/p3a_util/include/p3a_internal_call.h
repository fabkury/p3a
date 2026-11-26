// SPDX-License-Identifier: Apache-2.0
// Runs a function on a stack that is in internal RAM.
//
// Every p3a task except the display pipeline has its stack in PSRAM, and
// the IDF asserts when flash is memory-mapped from such a task
// (esp_partition_mmap, spi_flash_mmap, and what uses them: reading and
// writing the boot selection in esp_ota_*, esp_image_verify, the core-dump
// summary). Code that needs one of those at boot calls it from app_main's
// task. Code that needs one later (the firmware update) hands it here.
//
// One small long-lived task does the calls, one at a time, in the order
// asked. Keep what is handed over short: only the calls that map flash,
// not the work around them.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*p3a_internal_fn)(void *arg);

// Starts the task. Call once, early in app_main. Safe to call again.
esp_err_t p3a_internal_call_start(void);

// Runs fn(arg) on the internal-RAM stack and returns what it returned. The
// caller waits. From a task whose own stack is internal (app_main's at
// boot) fn runs right there. ESP_ERR_INVALID_STATE before the start.
// fn has 6 KB of stack and must not call p3a_internal_call() itself.
esp_err_t p3a_internal_call(p3a_internal_fn fn, void *arg);

#ifdef __cplusplus
}
#endif
