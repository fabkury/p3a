// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file view_tracker.h
 * @brief View tracker interface: timed artwork view reporting to Makapix
 */

#pragma once

#include "esp_err.h"
#include "play_scheduler_types.h"  // post_source_t
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
 * @brief Signal that a buffer swap occurred with artwork info
 *
 * Call this from the render task after a buffer swap. All fields are captured
 * at swap time so the view event reports the channel the post was actually
 * picked from, even when the playset's stochastic selection picks a different
 * channel on the next swap before the view event fires.
 *
 * @param post_id              The post ID of the artwork now playing (0 to stop tracking)
 * @param post_source          Source of the post_id (Makapix/Giphy/SDCARD)
 * @param filepath             The filepath of the artwork now playing (NULL to stop tracking)
 * @param channel_type         Channel the post was picked from
 * @param channel_spec_name    Channel sub-type ("all", "promoted", "user", "hashtag", "reactions", "sdcard", ...)
 * @param channel_identifier   USER/REACTIONS sqid or HASHTAG tag (empty otherwise)
 */
void view_tracker_signal_swap(int32_t post_id, post_source_t post_source, const char *filepath,
                              ps_channel_type_t channel_type,
                              const char *channel_spec_name,
                              const char *channel_identifier);

/**
 * @brief Stop tracking views
 * 
 * Stops the timer and clears tracking state. Use this when leaving Makapix channels
 * or when artwork playback stops.
 */
void view_tracker_stop(void);

/**
 * @brief Pause view tracking (ref-counted)
 *
 * Stops the dwell timer but preserves tracking state (elapsed time, post_id,
 * etc.). Multiple independent sources may pause concurrently — the timer is
 * stopped on the 0->1 ref-count edge, and only restarted once every paired
 * resume() has run. Each pause() MUST be matched by exactly one resume() from
 * the same source. Calls made while no artwork is currently being tracked
 * still count toward the ref-count, so resume() balances correctly.
 */
void view_tracker_pause(void);

/**
 * @brief Resume view tracking (ref-counted)
 *
 * Decrements the pause ref-count and restarts the dwell timer on the 1->0
 * edge. Tracking state is preserved across the pause/resume window. Unbalanced
 * resume() calls are logged and ignored.
 */
void view_tracker_resume(void);

#ifdef __cplusplus
}
#endif

