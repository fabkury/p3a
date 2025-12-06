#ifndef ANIMATION_PLAYER_H
#define ANIMATION_PLAYER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

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
void animation_player_cycle_animation(bool forward);

esp_err_t animation_player_start(void);
void animation_player_deinit(void);

size_t animation_player_get_current_index(void);
esp_err_t animation_player_add_file(const char *filename, const char *animations_dir, size_t insert_after_index, size_t *out_index);
esp_err_t animation_player_swap_to_index(size_t index);

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
esp_err_t animation_player_submit_pico8_frame(const uint8_t *palette_rgb, size_t palette_len,
                                              const uint8_t *pixel_data, size_t pixel_len);

// Screen rotation types (must match animation_player_priv.h)
typedef enum {
    ROTATION_0   = 0,
    ROTATION_90  = 90,
    ROTATION_180 = 180,
    ROTATION_270 = 270
} screen_rotation_t;

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

#ifdef __cplusplus
}
#endif

#endif // ANIMATION_PLAYER_H
