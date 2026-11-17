// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

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
#include "config_store.h"
#include "animation_player.h"
#include "display_renderer.h"
#include "play_scheduler.h"
#include "ugfx_ui.h"
#include "sd_format.h"
#include "p3a_boot_logo.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_lcd";

static int fatal_error_render_cb(uint8_t *buffer, void *ctx)
{
    size_t stride = p3a_board_get_row_stride();
    return ugfx_ui_render_to_buffer(buffer, stride);
}

esp_err_t app_lcd_init(void)
{
    ESP_LOGI(TAG, "P3A: Initialize display");

    // Step 1: Initialize board display hardware. Pass the configured background
    // color so the board prepaints the framebuffers to it before switching on
    // the backlight — the panel lights up already showing the background, never
    // a black/garbage frame, and the boot logo then fades in over it.
    uint8_t bg_r, bg_g, bg_b;
    config_store_get_background_color(&bg_r, &bg_g, &bg_b);
    esp_err_t err = p3a_board_display_init(bg_r, bg_g, bg_b);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize board display: %s", esp_err_to_name(err));
        return err;
    }

    // Step 1.5: Arm the boot logo. This only initializes logo state — the
    // display timer starts on the first rendered frame (once the render tasks
    // are running), not here. Initialization that runs after the render
    // pipeline starts (channel load, Wi-Fi, OTA) proceeds in parallel while
    // the logo is displayed.
    err = p3a_boot_logo_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot logo init failed: %s (continuing without logo)", esp_err_to_name(err));
        // Non-fatal - continue without boot logo
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

        // Re-initialize display renderer (torn down by animation_player_init cleanup)
        // and show a fatal error screen instead of crashing. This body is the
        // neutral (no-touch) variant: once sd_format_fatal_screen_loop() gets
        // touch running it swaps in a body that also offers the on-device
        // format button, so the text never promises a button that isn't there.
        static const char *fatal_title = "No Usable SD Card";
        static const char *fatal_body =
            "A working microSD card is required.\n\nPower off, unscrew the back plate,\nand insert a microSD card.";
        display_renderer_init(panel, buffers, buffer_count, buffer_bytes, row_stride);
        display_renderer_set_frame_callback(fatal_error_render_cb, NULL);
        ugfx_ui_show_fatal_error(fatal_title, fatal_body);
        display_renderer_start();

        ESP_LOGE(TAG, "FATAL: No usable microSD card (missing or unreadable). "
                      "Display showing error screen. Entering interactive format loop.");
        // Never returns: initializes touch itself and offers the opt-in
        // "Format card for p3a" flow (falls back to plain suspend if touch
        // is unavailable — the old behavior).
        sd_format_fatal_screen_loop(fatal_title, fatal_body);
        // Unreachable
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
    animation_player_set_paused(paused);
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
        return;
    }
    play_scheduler_next(NULL);
}

void app_lcd_cycle_animation_backward(void)
{
    if (animation_player_is_sd_export_locked()) {
        return;
    }
    play_scheduler_prev(NULL);
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
    esp_err_t err = p3a_board_set_brightness(brightness_percent);
    if (err == ESP_OK) {
        event_bus_emit_i32(P3A_EVENT_BRIGHTNESS_CHANGED,
                           p3a_board_get_brightness());
    }
    return err;
}

esp_err_t app_lcd_adjust_brightness(int delta_percent)
{
    esp_err_t err = p3a_board_adjust_brightness(delta_percent);
    if (err == ESP_OK) {
        event_bus_emit_i32(P3A_EVENT_BRIGHTNESS_CHANGED,
                           p3a_board_get_brightness());
    }
    return err;
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
    // The "SD card exposed via USB" notice is modal — it stays up for as long
    // as the card is exported. Refuse to leave UI mode while the export lock is
    // held so background events can't dismiss it and resume playback. The USB
    // unmount/suspend path drops the lock before calling here. See
    // animation_player_exit_ui_mode() for the authoritative guard.
    if (animation_player_is_sd_export_locked()) {
        ESP_LOGD(TAG, "Exit-UI-mode ignored: microSD card exported over USB");
        return ESP_OK;
    }
    // Likewise the SD-format flow is modal from the warning panel onward:
    // background events must not dismiss it and resume SD playback while the
    // user is deciding whether to erase the card. (During the format itself
    // the export lock above already refuses.)
    if (sd_format_is_active()) {
        ESP_LOGD(TAG, "Exit-UI-mode ignored: SD format flow active");
        return ESP_OK;
    }
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
