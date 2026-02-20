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
 * @brief Context for Giphy API fetch operations
 *
 * Bundles API configuration and a shared response buffer so that the caller
 * can reuse the same buffer across multiple paginated requests.
 */
typedef struct {
    char api_key[128];
    char rendition[32];
    char format[8];
    char rating[8];
    char query[64];             ///< Search query (empty = trending, non-empty = search)
    char *response_buf;         ///< Caller-allocated buffer (PSRAM recommended)
    size_t response_buf_size;   ///< Size of response_buf in bytes
} giphy_fetch_ctx_t;

/**
 * @brief Fetch a single page of GIFs from Giphy API (trending or search)
 *
 * When ctx->query is empty, fetches from /v1/gifs/trending.
 * When ctx->query is non-empty, fetches from /v1/gifs/search with that query.
 *
 * Builds the request URL, performs the HTTP GET, parses the JSON response,
 * and fills the output array with up to 25 entries. The caller is responsible
 * for pagination, cancellation checks, and inter-page delays.
 *
 * @param ctx        Fetch context (API config + shared response buffer)
 * @param offset     Pagination offset (0-based)
 * @param out_entries Output array (must hold at least 25 entries)
 * @param out_count  Number of entries parsed from this page
 * @param out_has_more Set to true if more pages are likely available
 * @return ESP_OK on success, ESP_ERR_NOT_ALLOWED on HTTP 401/403,
 *         ESP_ERR_INVALID_RESPONSE on HTTP 429, ESP_FAIL on other errors
 */
esp_err_t giphy_fetch_page(giphy_fetch_ctx_t *ctx, int offset,
                           giphy_channel_entry_t *out_entries,
                           size_t *out_count, bool *out_has_more);

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

/**
 * @brief Progress callback for giphy_download_artwork_with_progress()
 *
 * @param bytes_read Bytes downloaded so far
 * @param content_length Total content length from HTTP header
 * @param user_ctx User context pointer
 */
typedef void (*giphy_download_progress_cb_t)(size_t bytes_read, size_t content_length, void *user_ctx);

/**
 * @brief Download a Giphy artwork with progress reporting
 *
 * Same as giphy_download_artwork() but invokes progress_cb after each chunk.
 */
esp_err_t giphy_download_artwork_with_progress(const char *giphy_id, uint8_t extension,
                                               char *out_path, size_t out_len,
                                               giphy_download_progress_cb_t progress_cb,
                                               void *progress_ctx);

// ============================================================================
// Refresh (called from play_scheduler_refresh)
// ============================================================================

/**
 * @brief Refresh a Giphy channel by fetching GIFs and merging into cache
 *
 * Dispatches to the trending or search endpoint based on channel_id:
 *   - "giphy_trending"        -> trending endpoint
 *   - "giphy_search_{query}"  -> search endpoint with the given query
 *
 * Called from play_scheduler_refresh.c when a Giphy channel has
 * refresh_pending = true.
 *
 * @param channel_id The channel ID (e.g., "giphy_trending" or "giphy_search_cats")
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if WiFi not ready
 */
esp_err_t giphy_refresh_channel(const char *channel_id);

/**
 * @brief Progress callback for giphy_refresh_channel_with_progress()
 *
 * @param current_offset Entries fetched so far
 * @param cache_size     Target cache size (total expected)
 * @param user_ctx       User context pointer
 */
typedef void (*giphy_refresh_progress_cb_t)(int current_offset, int cache_size, void *user_ctx);

/**
 * @brief Refresh a Giphy channel with per-page progress reporting
 *
 * Same as giphy_refresh_channel() but invokes progress_cb after each page merge.
 */
esp_err_t giphy_refresh_channel_with_progress(const char *channel_id,
                                               giphy_refresh_progress_cb_t progress_cb,
                                               void *progress_ctx);

/**
 * @brief Cancel any in-progress Giphy refresh
 *
 * Sets a flag checked between pages in giphy_refresh_channel().
 * The in-flight HTTP request completes; cancellation takes effect at
 * the next check point. Safe to call from any task.
 */
void giphy_cancel_refresh(void);

/**
 * @brief Check if refresh cancellation has been requested
 */
bool giphy_is_refresh_cancelled(void);

#ifdef __cplusplus
}
#endif
