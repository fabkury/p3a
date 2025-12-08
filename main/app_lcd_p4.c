/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_lcd_p4.c
 * @brief Application-level display functions
 * 
 * This module provides high-level display operations for the p3a application.
 * It uses the p3a_board component for hardware access and manages
 * the animation player and UI mode.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"

#include "p3a_board.h"
#include "app_lcd.h"
#include "animation_player.h"
#include "ugfx_ui.h"

// Forward declaration for auto-swap timer reset
extern void auto_swap_reset_timer(void);

static const char *TAG = "app_lcd";

void app_lcd_draw(uint8_t *buf, uint32_t len, uint16_t width, uint16_t height)
{
    (void)buf;
    (void)len;
    (void)width;
    (void)height;
    // The animation owns the display pipeline; external draw requests are ignored.
}

esp_err_t app_lcd_init(void)
{
    ESP_LOGI(TAG, "P3A: Initialize display");

    // Step 1: Initialize board display hardware
    esp_err_t err = p3a_board_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize board display: %s", esp_err_to_name(err));
        return err;
    }

    // Step 2: Get hardware info from board component
    esp_lcd_panel_handle_t panel = p3a_board_get_panel();
    uint8_t buffer_count = p3a_board_get_buffer_count();
    size_t buffer_bytes = p3a_board_get_buffer_bytes();
    size_t row_stride = p3a_board_get_row_stride();

    // Build buffer array - MUST be static because display_renderer stores a pointer to it
    static uint8_t *buffers[P3A_DISPLAY_BUFFERS];
    for (int i = 0; i < buffer_count; i++) {
        buffers[i] = p3a_board_get_buffer(i);
    }

    // Step 3: Initialize animation player with hardware resources
    err = animation_player_init(panel, buffers, buffer_count, buffer_bytes, row_stride);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize animation player: %s", esp_err_to_name(err));
        return err;
    }

    // Step 4: Start animation player task
    err = animation_player_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start animation player: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Display initialized successfully");
    return ESP_OK;
}

// ============================================================================
// Animation control (application-level)
// ============================================================================

void app_lcd_set_animation_paused(bool paused)
{
    bool was_paused = animation_player_is_paused();
    animation_player_set_paused(paused);
    if (was_paused && !paused) {
        auto_swap_reset_timer();
    }
}

void app_lcd_toggle_animation_pause(void)
{
    animation_player_toggle_pause();
}

bool app_lcd_is_animation_paused(void)
{
    return animation_player_is_paused();
}

void app_lcd_cycle_animation(void)
{
    if (animation_player_is_sd_export_locked()) {
        ESP_LOGW(TAG, "Swap ignored while SD is exported over USB");
        return;
    }
    animation_player_cycle_animation(true);
    auto_swap_reset_timer();
}

void app_lcd_cycle_animation_backward(void)
{
    if (animation_player_is_sd_export_locked()) {
        ESP_LOGW(TAG, "Swap ignored while SD is exported over USB");
        return;
    }
    animation_player_cycle_animation(false);
    auto_swap_reset_timer();
}

// ============================================================================
// Brightness control (delegates to board component)
// ============================================================================

int app_lcd_get_brightness(void)
{
    return p3a_board_get_brightness();
}

esp_err_t app_lcd_set_brightness(int brightness_percent)
{
    return p3a_board_set_brightness(brightness_percent);
    }
    
esp_err_t app_lcd_adjust_brightness(int delta_percent)
{
    return p3a_board_adjust_brightness(delta_percent);
}

// ============================================================================
// UI mode control (application-level)
// ============================================================================

esp_err_t app_lcd_enter_ui_mode(void)
{
    if (animation_player_is_ui_mode()) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Entering UI mode");
    return animation_player_enter_ui_mode();
}

esp_err_t app_lcd_exit_ui_mode(void)
{
    if (!animation_player_is_ui_mode()) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Exiting UI mode");
    animation_player_exit_ui_mode();
    return ESP_OK;
}

bool app_lcd_is_ui_mode(void)
{
    return animation_player_is_ui_mode();
}

// ============================================================================
// Hardware access (delegates to board component)
// ============================================================================

uint8_t *app_lcd_get_framebuffer(int index)
{
    return p3a_board_get_buffer(index);
}

size_t app_lcd_get_row_stride(void)
{
    return p3a_board_get_row_stride();
}

esp_lcd_panel_handle_t app_lcd_get_panel_handle(void)
{
    return p3a_board_get_panel();
}
