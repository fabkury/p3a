/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

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
 * @brief Initialize animation player
 *
 * @param display_handle LCD panel handle
 * @param lcd_buffers Array of LCD frame buffers
 * @param buffer_count Number of buffers
 * @param buffer_bytes Size of each buffer in bytes
 * @param row_stride_bytes Row stride in bytes
 * @return ESP_OK on success
 */
esp_err_t animation_player_init(esp_lcd_panel_handle_t display_handle,
                                 uint8_t **lcd_buffers,
                                 uint8_t buffer_count,
                                 size_t buffer_bytes,
                                 size_t row_stride_bytes);

/**
 * @brief Load animation asset from file
 *
 * @param filepath Full path to animation file
 * @return ESP_OK on success
 */
esp_err_t animation_player_load_asset(const char *filepath);

/**
 * @brief Set animation paused state
 *
 * @param paused True to pause, false to resume
 */
void animation_player_set_paused(bool paused);

/**
 * @brief Toggle animation pause state
 */
void animation_player_toggle_pause(void);

/**
 * @brief Check if animation is paused
 *
 * @return True if paused
 */
bool animation_player_is_paused(void);

/**
 * @brief Cycle to next or previous animation in list
 *
 * @param forward True to cycle forward (next), false to cycle backward (previous)
 */
void animation_player_cycle_animation(bool forward);

/**
 * @brief Start animation player task
 *
 * @return ESP_OK on success
 */
esp_err_t animation_player_start(void);

/**
 * @brief Deinitialize animation player
 */
void animation_player_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // ANIMATION_PLAYER_H

