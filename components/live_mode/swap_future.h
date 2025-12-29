// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "playlist_manager.h"  // artwork_ref_t
#include "play_navigator.h"    // play_navigator_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Structure representing a scheduled future swap
 * 
 * A swap_future allows scheduling an animation swap to occur at a specific
 * wall-clock time, optionally starting at a specific frame for Live Mode sync.
 */
typedef struct {
    bool valid;                   // Whether this swap_future is active
    uint64_t target_time_ms;      // Wall-clock time (ms since epoch) to execute swap
    uint64_t start_time_ms;       // Ideal wall-clock time when this animation should have started (ms since epoch)
    uint32_t start_frame;         // Which frame to begin at (0-based, 0 = start from beginning)
    uint32_t live_index;          // Flattened Live Mode index (0..live_count-1). Undefined if not a Live Mode swap.
    artwork_ref_t artwork;        // Artwork to load and swap to
    bool is_live_mode_swap;       // Whether this swap maintains Live Mode synchronization
    bool is_automated;            // True for auto-swaps, false for manual swaps
} swap_future_t;

/**
 * @brief Initialize the swap_future system
 * 
 * Must be called before using any swap_future functions.
 * Creates necessary synchronization primitives.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t swap_future_init(void);

/**
 * @brief Deinitialize the swap_future system
 * 
 * Frees resources allocated by swap_future_init().
 */
void swap_future_deinit(void);

/**
 * @brief Schedule a future swap
 * 
 * Schedules an animation swap to occur at the specified target time.
 * Only one swap_future can be pending at a time. If a swap_future is
 * already scheduled, it will be replaced.
 * 
 * @param swap Swap future configuration
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_ARG if swap is NULL or invalid
 *         ESP_ERR_INVALID_STATE if swap_future system not initialized
 */
esp_err_t swap_future_schedule(const swap_future_t *swap);

/**
 * @brief Cancel any pending swap_future
 * 
 * Clears the currently scheduled swap_future if one exists.
 */
void swap_future_cancel(void);

/**
 * @brief Check if a swap_future is ready to execute
 * 
 * Compares the current time against the target time of the pending swap_future.
 * 
 * @param current_time_ms Current wall-clock time in milliseconds
 * @param out_swap Pointer to receive the swap_future if ready (can be NULL)
 * @return true if swap_future exists and current time >= target time
 */
bool swap_future_is_ready(uint64_t current_time_ms, swap_future_t *out_swap);

/**
 * @brief Get the currently scheduled swap_future
 * 
 * @param out_swap Pointer to receive the swap_future
 * @return ESP_OK if a swap_future is scheduled
 *         ESP_ERR_NOT_FOUND if no swap_future is pending
 */
esp_err_t swap_future_get_pending(swap_future_t *out_swap);

/**
 * @brief Check if any swap_future is currently scheduled
 * 
 * @return true if a swap_future is pending, false otherwise
 */
bool swap_future_has_pending(void);

/**
 * @brief Execute a pending swap_future
 * 
 * Triggers the animation loader to load the artwork specified in the swap_future.
 * This function should be called when swap_future_is_ready() returns true.
 * 
 * Note: This function requires animation_player internal state, so it's declared
 * here but should be implemented by animation_player.c
 * 
 * @param swap The swap_future to execute
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_ARG if swap is NULL
 *         ESP_ERR_INVALID_STATE if swap cannot be executed
 */
esp_err_t swap_future_execute(const swap_future_t *swap);

/**
 * @brief Enter Live Mode
 * 
 * Attempts to enter Live Mode by:
 * 1. Verifying preconditions (NTP sync, channel loaded)
 * 2. Building live schedule if needed
 * 3. Calculating current sync position
 * 4. Scheduling swap_future to jump to correct position
 * 
 * @param navigator Play navigator to enter Live Mode
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_STATE if preconditions not met
 *         ESP_ERR_NOT_FOUND if schedule cannot be built
 */
esp_err_t live_mode_enter(void *navigator);

/**
 * @brief Exit Live Mode
 * 
 * Exits Live Mode by:
 * 1. Clearing live_mode flag in navigator
 * 2. Canceling any pending swap_future
 * 3. Resuming normal playback
 * 
 * @param navigator Play navigator to exit Live Mode from
 */
void live_mode_exit(void *navigator);

/**
 * @brief Check if currently in Live Mode
 * 
 * @param navigator Play navigator to check
 * @return true if in Live Mode, false otherwise
 */
bool live_mode_is_active(void *navigator);

/**
 * @brief Live Mode helper: schedule the next automatic swap_future
 *
 * Calculates the next Live Mode transition time from wall clock, and schedules a swap_future
 * that will execute at the ideal boundary. Intended to be called by auto_swap_task.
 *
 * @param navigator Play navigator (play_navigator_t*)
 * @return ESP_OK if scheduled, ESP_ERR_INVALID_STATE if not in Live Mode, or other error.
 */
esp_err_t live_mode_schedule_next_swap(void *navigator);

/**
 * @brief Live Mode recovery: after a failed swap, skip forward to a decodable item
 *
 * Searches forward (wraparound) in the flattened live schedule up to a bounded scan depth,
 * and schedules an immediate swap_future to the first candidate whose file exists.
 *
 * Note: "available" is fully determined by loader+prefetch success; this function only chooses
 * candidates deterministically and triggers attempts. It does not block to decode.
 *
 * @param navigator Play navigator (play_navigator_t*)
 * @param failed_live_index Live schedule index that failed (0..live_count-1)
 * @param reason Error code for logging/backoff decisions
 */
esp_err_t live_mode_recover_from_failed_swap(void *navigator, uint32_t failed_live_index, esp_err_t reason);

/**
 * @brief Live Mode notification: swap succeeded
 *
 * Used to reset recovery backoff counters when a scheduled swap completes successfully.
 */
void live_mode_notify_swap_succeeded(uint32_t live_index);

#ifdef __cplusplus
}
#endif
