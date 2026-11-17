// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file playback_service.h
 * @brief High-level playback service interface
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t playback_service_init(void);
esp_err_t playback_service_play_channel(const char *channel_id);
esp_err_t playback_service_play_user_channel(const char *user_sqid);
esp_err_t playback_service_play_hashtag_channel(const char *hashtag);
esp_err_t playback_service_next(void);
esp_err_t playback_service_prev(void);
esp_err_t playback_service_pause(void);
esp_err_t playback_service_resume(void);
bool playback_service_is_paused(void);
esp_err_t playback_service_set_rotation(int degrees);

#ifdef __cplusplus
}
#endif
