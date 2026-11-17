// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file p3a_pin_dispatcher.h
 * @brief Single entry point for pin/unpin requests from gestures or HTTP.
 *
 * Each call:
 *   1. Resolves the artwork to pin from p3a_current_post (for the gesture path)
 *      or from explicit arguments (for the HTTP path).
 *   2. Shows the on-screen pin overlay optimistically.
 *   3. Spawns a fire-and-forget FreeRTOS task that copies the artwork bytes
 *      into the target list's vault and updates order.bin + manifest.
 *   4. On failure, the task overrides the overlay with the error icon.
 *
 * Phase 3 implements Makapix only; Giphy and museum sources return
 * ESP_ERR_NOT_SUPPORTED until phase 4 wires their resolution paths.
 */

#ifndef P3A_PIN_DISPATCHER_H
#define P3A_PIN_DISPATCHER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Pin the currently displayed artwork into the given list.
 *
 * The dispatcher reads p3a_current_post to learn the on-screen artwork's
 * source/post_id and the on-disk filepath of the loaded bytes.
 *
 * @param slug Target list slug, or NULL/"" for the active list.
 * @return ESP_OK on dispatch (the copy may still fail asynchronously);
 *         ESP_ERR_INVALID_STATE if nothing is displayed;
 *         ESP_ERR_NOT_SUPPORTED if the current source isn't supported yet.
 */
esp_err_t p3a_pin_dispatch_from_current(const char *slug);

/**
 * Unpin the currently displayed artwork from the given list.
 *
 * @param slug Target list slug, or NULL/"" for the active list.
 */
esp_err_t p3a_pin_dispatch_unpin_from_current(const char *slug);

/**
 * HTTP path: pin a specific Makapix post by post_id into a list.
 *
 * Caller is responsible for verifying that post_id matches the currently
 * displayed artwork (so the source path can be resolved from current_post).
 */
esp_err_t p3a_pin_dispatch_makapix_pin(int32_t post_id, const char *slug);

/**
 * HTTP path: unpin a specific Makapix post by post_id from a list.
 *
 * For Makapix, source_id is derived from the current_post filepath (which
 * is the on-disk UUID). The caller pre-validates post_id against current_post.
 */
esp_err_t p3a_pin_dispatch_makapix_unpin(int32_t post_id, const char *slug);

#ifdef __cplusplus
}
#endif

#endif // P3A_PIN_DISPATCHER_H
