#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "channel_interface.h"

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
    uint32_t start_frame;         // Which frame to begin at (0-based, 0 = start from beginning)
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

#ifdef __cplusplus
}
#endif
