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
 * @brief Signal that a buffer swap occurred (ZERO STACK USAGE)
 * 
 * Call this from the render task after a buffer swap. This function does
 * ZERO stack allocation and NO function calls - it just sets atomic flags.
 * The view tracker task will poll for changes and handle them.
 * 
 * This must be called from the render callback BEFORE any filepath becomes invalid.
 */
void view_tracker_signal_swap(void);

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

