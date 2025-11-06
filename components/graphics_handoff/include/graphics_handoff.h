#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enter player mode (LVGL off, direct LCD control)
 * 
 * This function:
 * 1. Stops/pauses LVGL rendering and timers
 * 2. Disables LVGL display flush callbacks
 * 3. Acquires panel handle from LVGL display context
 * 4. Returns panel handle for direct use
 * 
 * @param panel_out Output parameter for panel handle
 * @param trans_sem_out Output parameter for transfer semaphore (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t graphics_handoff_enter_player_mode(esp_lcd_panel_handle_t* panel_out, void** trans_sem_out);

/**
 * @brief Enter LVGL mode (resume LVGL, release LCD control)
 * 
 * This function:
 * 1. Signals player to finish current frame and quiesce DMA
 * 2. Releases LCD windowing/ownership
 * 3. Resumes LVGL rendering and timers
 * 4. Re-attaches panel to LVGL display driver
 * 5. Issues LVGL full refresh
 * 
 * @return ESP_OK on success
 */
esp_err_t graphics_handoff_enter_lvgl_mode(void);

/**
 * @brief Check if currently in player mode
 * 
 * @return true if in player mode, false if in LVGL mode
 */
bool graphics_handoff_is_player_mode(void);

/**
 * @brief Initialize graphics handoff system
 * 
 * Must be called before using handoff functions.
 * 
 * @return ESP_OK on success
 */
esp_err_t graphics_handoff_init(void);

#ifdef __cplusplus
}
#endif

