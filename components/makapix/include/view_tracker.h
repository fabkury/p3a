// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the view tracker
 * 
 * Creates the FreeRTOS timer and initializes state.
 * Must be called before any other view_tracker functions.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t view_tracker_init(void);

/**
 * @brief Deinitialize the view tracker
 * 
 * Stops the timer and frees resources.
 */
void view_tracker_deinit(void);

/**
 * @brief Notify the tracker of an animation change
 * 
 * This function should be called whenever a new animation starts playing.
 * If the animation is the same as the current one (redundant change), the timer
 * is NOT reset. Otherwise, the timer resets and starts tracking the new artwork.
 * 
 * @param post_id Post ID of the artwork (0 if not a Makapix artwork)
 * @param filepath Full path to the animation file
 * @param is_intentional True if this is from show_artwork command, false for channel playback
 */
void view_tracker_notify_animation_change(int32_t post_id, const char *filepath, bool is_intentional);

/**
 * @brief Stop tracking views
 * 
 * Stops the timer and clears tracking state. Use this when leaving Makapix channels
 * or when artwork playback stops.
 */
void view_tracker_stop(void);

#ifdef __cplusplus
}
#endif

