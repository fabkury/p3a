// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file p3a_state_channel.c
 * @brief Channel fallback (sdcard) helper.
 */

#include "p3a_state_internal.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "p3a_state_ch";

// Forward declarations for play_scheduler functions
extern esp_err_t play_scheduler_play_named_channel(const char *name) __attribute__((weak));
extern esp_err_t play_scheduler_refresh_sdcard_cache(void) __attribute__((weak));

// External UI function declarations
extern void p3a_render_set_channel_message(const char *channel_name, int msg_type, int progress_percent, const char *detail) __attribute__((weak));
extern esp_err_t ugfx_ui_show_channel_message(const char *channel_name, const char *message, int progress_percent) __attribute__((weak));

// External animation player function
extern bool animation_player_is_animation_ready(void) __attribute__((weak));

esp_err_t p3a_state_fallback_to_sdcard(void)
{
    ESP_LOGI(TAG, "Falling back to SD card channel");

    // Switch play_scheduler to sdcard channel
    esp_err_t err = ESP_OK;
    if (play_scheduler_play_named_channel) {
        err = play_scheduler_play_named_channel("sdcard");
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to switch play_scheduler to sdcard: %s", esp_err_to_name(err));
        }
    }
    if (play_scheduler_refresh_sdcard_cache) {
        play_scheduler_refresh_sdcard_cache();
    }

    // Check if SD card channel has any artworks
    bool has_animations = false;
    if (animation_player_is_animation_ready) {
        has_animations = animation_player_is_animation_ready();
    }

    if (!has_animations) {
        // SD card is also empty - show persistent "no artworks" message
        ESP_LOGW(TAG, "No artworks available on SD card either - showing empty message");
        if (p3a_render_set_channel_message) {
            p3a_render_set_channel_message("p3a", 4 /* P3A_CHANNEL_MSG_EMPTY */, -1,
                                          "No artworks to play");
        }
        if (ugfx_ui_show_channel_message) {
            ugfx_ui_show_channel_message("p3a", "No artworks to play", -1);
        }
    }

    return err;
}
