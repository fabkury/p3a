// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file sd_health.h
 * @brief SD card health tracking: boot-time probe + passive write-failure
 *        counting, latched failure state (sticky until reboot).
 *
 * Detection has two inputs:
 *  - An active boot probe (sd_health_boot_probe): write a small file, rename
 *    it, read it back, delete it. Any step failing latches SD_HEALTH_FAILED.
 *  - Passive reports from fs_atomic (and other SD writers): every SD write /
 *    rename outcome is reported; SD_HEALTH_FAIL_THRESHOLD consecutive
 *    failures latch SD_HEALTH_FAILED.
 *
 * Once latched the state never clears until reboot (the board has no
 * card-detect pin, so hot-swap cannot be detected reliably). On latch:
 *  - P3A_EVENT_SD_HEALTH_FAILED is emitted on the event bus (channel_manager
 *    flips its SD-unavailable gate, stopping downloads/cache flushes).
 *  - A pending-overlay flag is set; the render loop consumes it via
 *    sd_health_take_pending_overlay() to show the on-screen error exactly
 *    once per boot.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Consecutive SD write/rename failures that trip the latch. */
#define SD_HEALTH_FAIL_THRESHOLD 3

typedef enum {
    SD_HEALTH_UNKNOWN = 0,  /**< Before init/probe */
    SD_HEALTH_OK,           /**< Probe passed, no active failure streak */
    SD_HEALTH_FAILED,       /**< Latched until reboot */
} sd_health_state_t;

/** Initialize state. Call once, before the boot probe. */
esp_err_t sd_health_init(void);

/**
 * Active self-test of the mounted SD card. Writes {root}/.sdh_probe.tmp,
 * fsyncs, renames to {root}/.sdh_probe, reads it back, verifies the pattern
 * and deletes it. Any failure latches SD_HEALTH_FAILED and returns ESP_FAIL.
 * Call after sd_path_init() + sd_path_ensure_directories().
 */
esp_err_t sd_health_boot_probe(void);

/** SD mount itself failed: latch immediately (same UX as a probe failure). */
void sd_health_report_mount_failed(void);

/**
 * Passive outcome reports. Normally called only by fs_atomic. Success resets
 * the consecutive-failure counter; failure increments it and latches at
 * SD_HEALTH_FAIL_THRESHOLD. Paths outside the SD mount prefix are ignored,
 * as is errno ENOSPC (disk-full is not card failure). Thread-safe.
 */
void sd_health_report_write_ok(const char *path);
void sd_health_report_write_failure(const char *path, int err_no);

/** Fast latch check (single flag read); callable from any task. */
bool sd_health_is_failed(void);

sd_health_state_t sd_health_get_state(void);

/**
 * For the render loop only: returns true exactly once after the latch trips
 * (consumes the pending-overlay flag), so the on-screen error shows once per
 * boot by construction.
 */
bool sd_health_take_pending_overlay(void);

#ifdef __cplusplus
}
#endif
