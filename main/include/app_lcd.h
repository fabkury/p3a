/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_lcd.h
 * @brief Application-level display interface
 * 
 * This header provides the application-level display API for p3a.
 * For hardware constants and low-level access, see p3a_board.h.
 */

#ifndef APP_LCD_H
#define APP_LCD_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "p3a_board.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// DISPLAY CONSTANTS (from board component)
// ============================================================================

// These are provided by p3a_board.h for backward compatibility:
// - EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES
// - EXAMPLE_LCD_BUF_NUM, EXAMPLE_LCD_BIT_PER_PIXEL, EXAMPLE_LCD_BUF_LEN
// - BSP_LCD_H_RES, BSP_LCD_V_RES
// - APP_LCD_MAX_SPEED_PLAYBACK_ENABLED

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize the display system
 * 
 * Initializes board display hardware and starts the animation player.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_lcd_init(void);

/**
 * @brief Draw to display (legacy, not used)
 */
void app_lcd_draw(uint8_t *buf, uint32_t len, uint16_t width, uint16_t height);

// ============================================================================
// ANIMATION CONTROL
// ============================================================================

/**
 * @brief Set animation paused state
 */
void app_lcd_set_animation_paused(bool paused);

/**
 * @brief Toggle animation pause state
 */
void app_lcd_toggle_animation_pause(void);

/**
 * @brief Check if animation is paused
 */
bool app_lcd_is_animation_paused(void);

/**
 * @brief Cycle to next animation
 */
void app_lcd_cycle_animation(void);

/**
 * @brief Cycle to previous animation
 */
void app_lcd_cycle_animation_backward(void);

// ============================================================================
// BRIGHTNESS CONTROL
// ============================================================================

/**
 * @brief Get current display brightness
 * 
 * @return Current brightness percentage (0-100)
 */
int app_lcd_get_brightness(void);

/**
 * @brief Set display brightness
 * 
 * @param brightness_percent Brightness percentage (0-100), will be clamped
 * @return ESP_OK on success
 */
esp_err_t app_lcd_set_brightness(int brightness_percent);

/**
 * @brief Adjust brightness by delta
 * 
 * @param delta_percent Change in brightness (can be negative)
 * @return ESP_OK on success
 */
esp_err_t app_lcd_adjust_brightness(int delta_percent);

// ============================================================================
// UI MODE
// ============================================================================

/**
 * @brief Enter UI mode (pause animation for UI rendering)
 */
esp_err_t app_lcd_enter_ui_mode(void);

/**
 * @brief Exit UI mode (resume animation)
 */
esp_err_t app_lcd_exit_ui_mode(void);

/**
 * @brief Check if UI mode is active
 */
bool app_lcd_is_ui_mode(void);

// ============================================================================
// HARDWARE ACCESS (delegates to board component)
// ============================================================================

/**
 * @brief Get framebuffer pointer
 * 
 * @param index Buffer index (0 to P3A_DISPLAY_BUFFERS-1)
 * @return Pointer to framebuffer, or NULL if invalid
 */
uint8_t *app_lcd_get_framebuffer(int index);

/**
 * @brief Get framebuffer row stride
 * 
 * @return Row stride in bytes
 */
size_t app_lcd_get_row_stride(void);

/**
 * @brief Get LCD panel handle
 * 
 * @return Panel handle, or NULL if not initialized
 */
esp_lcd_panel_handle_t app_lcd_get_panel_handle(void);

#ifdef __cplusplus
}
#endif

#endif // APP_LCD_H
