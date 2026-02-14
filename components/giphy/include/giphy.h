// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "giphy_types.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Initialization
// ============================================================================

/**
 * @brief Initialize the Giphy component
 *
 * @return ESP_OK on success
 */
esp_err_t giphy_init(void);

/**
 * @brief Deinitialize the Giphy component
 */
void giphy_deinit(void);

// ============================================================================
// API Client
// ============================================================================

/**
 * @brief Fetch trending GIFs from Giphy API
 *
 * Paginates internally (limit=50 per request) up to max_entries.
 * Uses configured API key, rating, rendition, and format from config_store.
 *
 * @param out_entries Output array of entries
 * @param max_entries Maximum number of entries to fetch
 * @param out_count Actual number of entries fetched
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no API key configured,
 *         ESP_ERR_NOT_ALLOWED on invalid API key (HTTP 401/403),
 *         ESP_ERR_INVALID_RESPONSE on rate limiting (HTTP 429),
 *         ESP_ERR_TIMEOUT on network timeout, ESP_FAIL on other API error
 */
esp_err_t giphy_fetch_trending(giphy_channel_entry_t *out_entries,
                               size_t max_entries, size_t *out_count);

// ============================================================================
// Cache / Path Helpers
// ============================================================================

/**
 * @brief Build filepath for a Giphy artwork on SD card
 *
 * Path format: /sdcard/p3a/giphy/{sha[0]}/{sha[1]}/{sha[2]}/{giphy_id}.{ext}
 * where sha = SHA256(giphy_id).
 *
 * @param giphy_id Giphy string ID
 * @param extension Extension index (0=webp, 1=gif)
 * @param out_path Output buffer
 * @param out_len Output buffer length
 * @return ESP_OK on success
 */
esp_err_t giphy_build_filepath(const char *giphy_id, uint8_t extension,
                               char *out_path, size_t out_len);

/**
 * @brief Build filepath for a giphy_channel_entry_t
 *
 * Convenience wrapper that extracts giphy_id and extension from the entry.
 *
 * @param entry Giphy channel entry (cast from makapix_channel_entry_t*)
 * @param out_path Output buffer
 * @param out_len Output buffer length
 */
void giphy_build_entry_filepath(const giphy_channel_entry_t *entry,
                                char *out_path, size_t out_len);

/**
 * @brief Convert a Giphy string ID to an int32_t post_id
 *
 * Uses salted DJB2 hash (salt=0x47495048), masked to negative range.
 *
 * @param giphy_id Giphy string ID (e.g., "YsTs5ltWtEhnq")
 * @return Negative int32_t post_id
 */
int32_t giphy_id_to_post_id(const char *giphy_id);

/**
 * @brief Check if a channel_id belongs to Giphy
 *
 * @param channel_id Channel ID string
 * @return true if channel_id starts with "giphy_"
 */
bool giphy_is_giphy_channel(const char *channel_id);

// ============================================================================
// Download
// ============================================================================

/**
 * @brief Build download URL for a Giphy artwork
 *
 * Reconstructs the URL from giphy_id + configured rendition/format.
 * Pattern: https://i.giphy.com/media/{giphy_id}/{rendition_suffix}
 *
 * @param giphy_id Giphy string ID
 * @param out_url Output buffer for the full URL
 * @param out_len Output buffer length
 * @return ESP_OK on success
 */
esp_err_t giphy_build_download_url(const char *giphy_id, char *out_url, size_t out_len);

/**
 * @brief Download a Giphy artwork to the giphy/ folder on SD card
 *
 * Downloads to a temp file, then atomically renames.
 * Creates sharded directories as needed.
 *
 * @param giphy_id Giphy string ID
 * @param extension Extension index (0=webp, 1=gif)
 * @param out_path Output buffer for the final file path
 * @param out_len Output buffer length
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND on 404,
 *         ESP_FAIL on other errors
 */
esp_err_t giphy_download_artwork(const char *giphy_id, uint8_t extension,
                                 char *out_path, size_t out_len);

// ============================================================================
// Refresh (called from play_scheduler_refresh)
// ============================================================================

/**
 * @brief Refresh a Giphy channel by fetching trending and merging into cache
 *
 * Called from play_scheduler_refresh.c when a Giphy channel has
 * refresh_pending = true.
 *
 * @param channel_id The channel ID (e.g., "giphy_trending")
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if WiFi not ready
 */
esp_err_t giphy_refresh_channel(const char *channel_id);

#ifdef __cplusplus
}
#endif
