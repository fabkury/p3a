// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_err.h"
#include <time.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Per-channel metadata persisted as a JSON sidecar file
 *
 * Stored alongside the binary .cache file at:
 *   {channels_path}/{channel_id}.json
 *
 * Used by both Makapix and Giphy channels to track refresh state.
 */
typedef struct {
    time_t last_refresh;    ///< Unix timestamp of last successful server query (0 = never)
    char cursor[64];        ///< Pagination cursor (empty string if unused)
} channel_metadata_t;

/**
 * @brief Save channel metadata to disk (atomic write)
 *
 * Writes JSON to {channels_path}/{channel_id}.json using atomic
 * temp-file-then-rename pattern.
 *
 * @param channel_id  Channel identifier (e.g., "all", "giphy_trending")
 * @param channels_path  Base directory (e.g., "/sdcard/p3a/channel")
 * @param meta  Metadata to persist
 * @return ESP_OK on success
 */
esp_err_t channel_metadata_save(const char *channel_id,
                                const char *channels_path,
                                const channel_metadata_t *meta);

/**
 * @brief Load channel metadata from disk
 *
 * Reads JSON from {channels_path}/{channel_id}.json.
 * If the file does not exist, out_meta is zeroed and ESP_ERR_NOT_FOUND
 * is returned (callers can treat this as "never refreshed").
 *
 * @param channel_id  Channel identifier
 * @param channels_path  Base directory
 * @param[out] out_meta  Loaded metadata (zeroed if file missing)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no file
 */
esp_err_t channel_metadata_load(const char *channel_id,
                                const char *channels_path,
                                channel_metadata_t *out_meta);

#ifdef __cplusplus
}
#endif
