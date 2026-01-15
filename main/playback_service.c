// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "playback_service.h"
#include "play_scheduler.h"
#include "animation_player.h"

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
    animation_player_set_paused(true);
    return ESP_OK;
}

esp_err_t playback_service_resume(void)
{
    animation_player_set_paused(false);
    return ESP_OK;
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
