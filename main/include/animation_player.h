// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef ANIMATION_PLAYER_H
#define ANIMATION_PLAYER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "display_renderer.h"
#include "sdcard_channel.h"  // For asset_type_t
#include "animation_swap_request.h"  // swap_request_t (shared with play_scheduler)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enter UI mode - animation task will call UI render function instead of animation
 * 
 * Sets render mode to UI. The animation task continues running but calls
 * ugfx_ui_render_to_buffer() instead of rendering animation frames.
 * Blocks until the render loop acknowledges the mode switch.
 */
esp_err_t animation_player_enter_ui_mode(void);

/**
 * @brief Exit UI mode - animation task resumes normal animation rendering
 * 
 * Sets render mode back to animation.
 * Blocks until the render loop acknowledges the mode switch.
 */
void animation_player_exit_ui_mode(void);

/**
 * @brief Check if currently in UI mode
 */
bool animation_player_is_ui_mode(void);

esp_err_t animation_player_init(esp_lcd_panel_handle_t display_handle,
                                uint8_t **lcd_buffers,
                                uint8_t buffer_count,
                                size_t buffer_bytes,
                                size_t row_stride_bytes);

esp_err_t animation_player_load_asset(const char *filepath);

void animation_player_set_paused(bool paused);
void animation_player_toggle_pause(void);
bool animation_player_is_paused(void);

// ============================================================================
// NEW SIMPLIFIED API (Phase 1 Refactor)
// ============================================================================

/**
 * @brief Request a validated swap to a specific artwork
 * 
 * This is the new simplified API. animation_player acts as a naive renderer
 * that either succeeds in the transition or displays an error message.
 * NO navigation logic, NO auto-retry, NO skipping.
 * 
 * @param request Pre-validated swap request from play_scheduler
 * @return ESP_OK if swap request accepted
 *         ESP_ERR_INVALID_STATE if swap already in progress
 */
esp_err_t animation_player_request_swap(const swap_request_t *request);

/**
 * @brief Display a message overlaid on the screen
 * 
 * Used to show error messages or channel status without clearing
 * the currently displayed artwork.
 * 
 * @param title Message title (NULL for no title)
 * @param body Message body
 */
void animation_player_display_message(const char *title, const char *body);

// ============================================================================
// DEPRECATED API (to be removed after refactor)
// ============================================================================

/**
 * @deprecated Use play_scheduler_next/play_scheduler_prev instead
 */
void animation_player_cycle_animation(bool forward);

/**
 * @deprecated Use play_scheduler_play_named_channel instead
 */
esp_err_t animation_player_request_swap_current(void);

// ============================================================================

esp_err_t animation_player_start(void);
void animation_player_deinit(void);

size_t animation_player_get_current_index(void);
esp_err_t animation_player_add_file(const char *filename, const char *animations_dir, size_t insert_after_index, size_t *out_index);
esp_err_t animation_player_swap_to_index(size_t index);

/**
 * @brief Request a swap to load the current channel item
 * 
 * Triggers the animation loader to load the current item from the active channel
 * (either sdcard_channel or makapix_channel) without advancing to the next item.
 * Useful when switching channels to immediately display the first item.
 * 
 * @return ESP_OK on success, error if swap already in progress or no items
 */
esp_err_t animation_player_request_swap_current(void);

esp_err_t animation_player_begin_sd_export(void);
esp_err_t animation_player_end_sd_export(void);
bool animation_player_is_sd_export_locked(void);

/**
 * @brief Check if the animation loader is currently busy loading from SD card
 * 
 * Used to coordinate with OTA to avoid SDIO bus contention.
 * 
 * @return true if loader is busy, false otherwise
 */
bool animation_player_is_loader_busy(void);

/**
 * @brief Pause SD card access for external operations
 * 
 * Call this before WiFi HTTPS operations to prevent SDIO bus conflicts.
 * While paused, animation swaps will be queued but not executed.
 * Remember to call animation_player_resume_sd_access() when done.
 */
void animation_player_pause_sd_access(void);

/**
 * @brief Resume SD card access after external operations
 * 
 * Call this after WiFi HTTPS operations complete.
 */
void animation_player_resume_sd_access(void);

/**
 * @brief Check if SD card access is currently paused
 * 
 * @return true if paused, false otherwise
 */
bool animation_player_is_sd_paused(void);

/**
 * @brief Check if an animation is ready to play
 * 
 * Returns true only when an animation has been successfully loaded
 * into the front buffer and is ready for playback.
 * 
 * @return true if animation is ready, false if still loading or no animation loaded
 */
bool animation_player_is_animation_ready(void);

/**
 * @brief Submit a PICO-8 frame for rendering
 * 
 * @deprecated Use pico8_render_submit_frame() instead
 */
esp_err_t animation_player_submit_pico8_frame(const uint8_t *palette_rgb, size_t palette_len,
                                              const uint8_t *pixel_data, size_t pixel_len);

// Screen rotation types - use display_renderer types
typedef display_rotation_t screen_rotation_t;
#define ROTATION_0   DISPLAY_ROTATION_0
#define ROTATION_90  DISPLAY_ROTATION_90
#define ROTATION_180 DISPLAY_ROTATION_180
#define ROTATION_270 DISPLAY_ROTATION_270

/**
 * @brief Set screen rotation for entire display
 * 
 * Applies rotation to both animation playback and UI rendering.
 * Animation rotation takes effect on next animation load (1-2 frames).
 * UI rotation takes effect immediately.
 * 
 * If a rotation operation is already in progress, this function returns
 * ESP_ERR_INVALID_STATE immediately without changing rotation.
 * 
 * @param rotation Rotation angle (0°, 90°, 180°, 270°)
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_ARG if rotation angle is invalid
 *         ESP_ERR_INVALID_STATE if rotation operation already in progress
 */
esp_err_t app_set_screen_rotation(screen_rotation_t rotation);

/**
 * @brief Get current screen rotation
 * 
 * @return Current rotation angle
 */
screen_rotation_t app_get_screen_rotation(void);

/**
 * @brief Get current dwell time setting
 * 
 * Returns the configured dwell time (how long to display still images before auto-advancing).
 * 
 * @return Dwell time in seconds (1-100000, default 30)
 */
uint32_t animation_player_get_dwell_time(void);

/**
 * @brief Set dwell time setting
 * 
 * Configures how long to display still images before auto-advancing to the next artwork.
 * The value is persisted to NVS and takes effect immediately.
 * 
 * @param dwell_time Dwell time in seconds (1-100000)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if value out of range
 */
esp_err_t animation_player_set_dwell_time(uint32_t dwell_time);

#ifdef __cplusplus
}
#endif

#endif // ANIMATION_PLAYER_H
