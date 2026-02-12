// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef CHANNEL_SETTINGS_H
#define CHANNEL_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool play_order_present;
    uint8_t play_order; // 0..2

    bool randomize_playlist_present;
    bool randomize_playlist;

    // Channel dwell override (0 disables this override)
    bool channel_dwell_time_present;
    uint32_t channel_dwell_time_ms;
} channel_settings_t;

/**
 * @brief Load per-channel settings from SD card.
 *
 * Makapix: <channel_dir>/<id>.settings.json
 * SD card: <channel_dir>/sdcard-channel.settings.json
 * 
 * The channel_dir is determined by sd_path_get_channel() (e.g., /sdcard/p3a/channel or custom).
 */
esp_err_t channel_settings_load_for_channel_id(const char *channel_id, channel_settings_t *out);
esp_err_t channel_settings_load_for_sdcard(channel_settings_t *out);

#ifdef __cplusplus
}
#endif

#endif // CHANNEL_SETTINGS_H

