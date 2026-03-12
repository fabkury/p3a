// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file p3a_state_channel.c
 * @brief Channel management: switch_channel, show_artwork, fallback_to_sdcard
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

// ============================================================================
// Channel Management
// ============================================================================

esp_err_t p3a_state_switch_channel(p3a_channel_type_t type, const char *identifier)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;

    char display_name_copy[64];

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    // Update channel info
    s_state.current_channel.type = type;

    if (identifier && (type == P3A_CHANNEL_MAKAPIX_BY_USER || type == P3A_CHANNEL_MAKAPIX_HASHTAG)) {
        snprintf(s_state.current_channel.identifier, sizeof(s_state.current_channel.identifier),
                 "%s", identifier);
    } else {
        memset(s_state.current_channel.identifier, 0, sizeof(s_state.current_channel.identifier));
    }

    memset(s_state.current_channel.storage_key, 0, sizeof(s_state.current_channel.storage_key));
    p3a_state_update_channel_display_name(&s_state.current_channel);

    // Copy display name for logging after mutex release
    snprintf(display_name_copy, sizeof(display_name_copy), "%s", s_state.current_channel.display_name);

    xSemaphoreGive(s_state.mutex);

    ESP_LOGI(TAG, "Switched to channel: %s", display_name_copy);

    return ESP_OK;
}

esp_err_t p3a_state_show_artwork(const char *storage_key, const char *art_url, int32_t post_id)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (!storage_key || !art_url) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    // Create transient artwork channel
    s_state.current_channel.type = P3A_CHANNEL_MAKAPIX_ARTWORK;
    memset(s_state.current_channel.identifier, 0, sizeof(s_state.current_channel.identifier));
    snprintf(s_state.current_channel.storage_key, sizeof(s_state.current_channel.storage_key),
             "%s", storage_key);
    p3a_state_update_channel_display_name(&s_state.current_channel);

    xSemaphoreGive(s_state.mutex);

    // Note: Do NOT persist artwork channels - they are transient

    ESP_LOGI(TAG, "Showing single artwork: %s (post_id=%ld)", storage_key, (long)post_id);

    return ESP_OK;
}

esp_err_t p3a_state_fallback_to_sdcard(void)
{
    ESP_LOGI(TAG, "Falling back to SD card channel");

    esp_err_t err = p3a_state_switch_channel(P3A_CHANNEL_SDCARD, NULL);

    // Switch play_scheduler to sdcard channel
    if (play_scheduler_play_named_channel) {
        esp_err_t ps_err = play_scheduler_play_named_channel("sdcard");
        if (ps_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to switch play_scheduler to sdcard: %s", esp_err_to_name(ps_err));
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
