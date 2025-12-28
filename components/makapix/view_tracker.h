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
 * @brief Signal that a buffer swap occurred with artwork info
 * 
 * Call this from the render task after a buffer swap. The post_id and filepath
 * are captured at swap time to ensure correct view tracking even if the channel
 * navigator advances before the view tracker processes the event.
 * 
 * @param post_id   The post ID of the artwork now playing (0 to stop tracking)
 * @param filepath  The filepath of the artwork now playing (NULL to stop tracking)
 */
void view_tracker_signal_swap(int32_t post_id, const char *filepath);

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

