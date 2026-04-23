// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file playback_controller.h
 * @brief Playback source controller: arbitrates between animation and PICO-8
 */

#ifndef PLAYBACK_CONTROLLER_H
#define PLAYBACK_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Active playback source type
 *
 * - NONE: No playback active (UI mode or idle)
 * - PICO8_STREAM: Real-time PICO-8 streaming from WiFi or USB
 * - LOCAL_ANIMATION: Local animation file playback from SD card
 */
typedef enum {
    PLAYBACK_SOURCE_NONE,
    PLAYBACK_SOURCE_PICO8_STREAM,
    PLAYBACK_SOURCE_LOCAL_ANIMATION
} playback_source_t;

esp_err_t playback_controller_init(void);
void playback_controller_deinit(void);

/** Current playback source. */
playback_source_t playback_controller_get_source(void);

/** Switch to PICO-8 streaming mode. Preserves the pending-animation flag so
 *  exit_pico8_mode can resume local playback afterwards. */
esp_err_t playback_controller_enter_pico8_mode(void);

/** Exit PICO-8 streaming. Resumes LOCAL_ANIMATION if one was pending, else
 *  drops to NONE. */
void playback_controller_exit_pico8_mode(void);

/** true iff current source is PICO8_STREAM. */
bool playback_controller_is_pico8_active(void);

/** Called by animation_player after a successful buffer swap. Marks that a
 *  local animation is active; flips current_source to LOCAL_ANIMATION unless
 *  PICO-8 mode is holding it. */
void playback_controller_notify_animation_active(void);

/** Called by animation_player when animation playback has been unloaded
 *  (e.g. entering UI mode for provisioning). Clears the pending-animation
 *  flag and drops current_source to NONE unless PICO-8 mode is active. */
void playback_controller_notify_animation_stopped(void);

#ifdef __cplusplus
}
#endif

#endif // PLAYBACK_CONTROLLER_H
