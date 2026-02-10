// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "playback_service.h"
#include "play_scheduler.h"
#include "animation_player.h"
#include "app_lcd.h"
#include "view_tracker.h"
#include "esp_log.h"

static const char *TAG = "playback_svc";

// Pause state (runtime-only, NOT persisted to NVS)
static bool s_paused = false;
static int  s_saved_brightness = 100;

esp_err_t playback_service_init(void)
{
    return play_scheduler_init();
}

esp_err_t playback_service_play_channel(const char *channel_id)
{
    if (!channel_id) {
        return ESP_ERR_INVALID_ARG;
    }
    return play_scheduler_play_named_channel(channel_id);
}

esp_err_t playback_service_play_user_channel(const char *user_sqid)
{
    return play_scheduler_play_user_channel(user_sqid);
}

esp_err_t playback_service_play_hashtag_channel(const char *hashtag)
{
    return play_scheduler_play_hashtag_channel(hashtag);
}

esp_err_t playback_service_next(void)
{
    return play_scheduler_next(NULL);
}

esp_err_t playback_service_prev(void)
{
    return play_scheduler_prev(NULL);
}

esp_err_t playback_service_pause(void)
{
    if (s_paused) {
        return ESP_OK;  // Already paused
    }

    // Save current brightness before blanking the screen
    s_saved_brightness = app_lcd_get_brightness();
    if (s_saved_brightness == 0) {
        // Edge case: if brightness was already 0, restore to a sane default
        s_saved_brightness = 100;
    }

    ESP_LOGI(TAG, "Pausing playback (saved brightness=%d)", s_saved_brightness);

    // Set animation paused flag (render callback will output black)
    animation_player_set_paused(true);

    // Pause view tracking (state is preserved)
    view_tracker_pause();

    // Set brightness to 0 for maximum blackness (backlight off)
    app_lcd_set_brightness(0);

    // Stop the auto-swap timer so no automatic swaps fire while paused
    play_scheduler_pause_auto_swap();

    s_paused = true;
    return ESP_OK;
}

esp_err_t playback_service_resume(void)
{
    if (!s_paused) {
        return ESP_OK;  // Not paused
    }

    ESP_LOGI(TAG, "Resuming playback (restoring brightness=%d)", s_saved_brightness);

    s_paused = false;

    // Resume animation decoding
    animation_player_set_paused(false);

    // Resume view tracking
    view_tracker_resume();

    // Restore user brightness
    app_lcd_set_brightness(s_saved_brightness);

    // Restart the auto-swap timer
    play_scheduler_resume_auto_swap();

    return ESP_OK;
}

bool playback_service_is_paused(void)
{
    return s_paused;
}

esp_err_t playback_service_set_rotation(int degrees)
{
    screen_rotation_t rotation;
    switch (degrees) {
        case 0:
            rotation = ROTATION_0;
            break;
        case 90:
            rotation = ROTATION_90;
            break;
        case 180:
            rotation = ROTATION_180;
            break;
        case 270:
            rotation = ROTATION_270;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    return app_set_screen_rotation(rotation);
}
