#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enter player mode
 * 
 * Switches to player mode (high-speed animation playback).
 * This is a state change notification - actual handoff is handled by graphics_handoff.
 */
void ui_mode_switch_enter_player_mode(void);

/**
 * @brief Enter LVGL mode
 * 
 * Switches to LVGL mode (standard UI rendering).
 * This is a state change notification - actual handoff is handled by graphics_handoff.
 */
void ui_mode_switch_enter_lvgl_mode(void);

/**
 * @brief Check if currently in player mode
 * 
 * @return true if in player mode, false if in LVGL mode
 */
bool ui_mode_switch_is_player_mode(void);

/**
 * @brief Initialize mode switch system
 * 
 * Must be called before using mode switch functions.
 * 
 * @return ESP_OK on success
 */
esp_err_t ui_mode_switch_init(void);

/**
 * @brief Register touch handle for long-press detection
 * 
 * @param touch_handle Touch controller handle from esp_lcd_touch
 */
void ui_mode_switch_register_touch(void* touch_handle);

/**
 * @brief Start touch polling task for long-press detection
 * 
 * Must be called after registering touch handle.
 * 
 * @return ESP_OK on success
 */
esp_err_t ui_mode_switch_start_touch_polling(void);

/**
 * @brief Set callbacks for mode switching
 * 
 * @param on_player Callback when entering player mode
 * @param on_lvgl Callback when entering LVGL mode
 */
void ui_mode_switch_set_callbacks(void (*on_player)(void), void (*on_lvgl)(void));

#ifdef __cplusplus
}
#endif

