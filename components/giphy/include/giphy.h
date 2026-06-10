// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file giphy.h
 * @brief Giphy component public API: init, trending/search, download, cache helpers
 */

#pragma once

#include "giphy_types.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/// Maximum items per Giphy API call.
#define GIPHY_PAGE_LIMIT 50

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
    bool prefer_downsized;      ///< When true and rendition is fixed_height, prefer downsized_medium
    uint16_t screen_width;      ///< Display width in pixels (for size-gating downsized_medium)
    uint16_t screen_height;     ///< Display height in pixels (for size-gating downsized_medium)
    char random_id[40];         ///< Giphy random_id for personalization (empty = omit)
    char country_code[4];       ///< 2-letter ISO 3166-1 country code (empty = omit)
} giphy_fetch_ctx_t;

/**
 * @brief Fetch a random_id from Giphy API for content personalization
 *
 * Calls GET /v1/randomid. On failure, sets out_random_id[0] = '\0'.
 *
 * @param api_key       Giphy API key
 * @param out_random_id Output buffer for the random_id string
 * @param max_len       Size of out_random_id buffer
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t giphy_fetch_random_id(const char *api_key, char *out_random_id, size_t max_len);

/**
 * @brief Register an onclick analytics event for a Giphy artwork
 *
 * Two-step pure-lazy flow:
 *   1. GET /v1/gifs/<giphy_id>?api_key=&fields=id,analytics&random_id=
 *      → parse data.analytics.onclick.url (a pre-signed pingback URL).
 *   2. GET <onclick_url>&random_id=&ts= → expects HTTP 200.
 *
 * Both calls are short and synchronous; intended to run from a short-lived
 * background task spawned in response to a user gesture.
 *
 * @param api_key   Giphy API key (required)
 * @param random_id Giphy random_id for personalization (required)
 * @param giphy_id  Giphy string ID of the artwork being clicked
 * @return ESP_OK if the pingback was accepted (HTTP 200);
 *         ESP_ERR_INVALID_ARG on missing inputs;
 *         ESP_ERR_NOT_ALLOWED on HTTP 401/403 (bad API key);
 *         ESP_ERR_INVALID_RESPONSE on HTTP 429 (quota exhausted);
 *         ESP_FAIL on any other error.
 */
esp_err_t giphy_register_click(const char *api_key,
                               const char *random_id,
                               const char *giphy_id);

/**
 * @brief Fetch a single page of GIFs from Giphy API (trending or search)
 *
 * When ctx->query is empty, fetches from /v1/gifs/trending.
 * When ctx->query is non-empty, fetches from /v1/gifs/search with that query.
 *
 * Builds the request URL, performs the HTTP GET, parses the JSON response,
 * and fills the output array with up to GIPHY_PAGE_LIMIT entries. The caller is responsible
 * for pagination, cancellation checks, and inter-page delays.
 *
 * @param ctx        Fetch context (API config + shared response buffer)
 * @param offset     Pagination offset (0-based)
 * @param out_entries Output array (must hold at least GIPHY_PAGE_LIMIT entries)
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
 * Path format: /sdcard/p3a/giphy/{d0}/{d1}/{giphy_id}.{ext}
 * where the d_i are the 6-bit decimal shard dirs derived from the giphy_id.
 * Built via the shared sd_path_build_sharded() helper, so the shard scheme
 * (SD_SHARD_DEPTH / SD_SHARD_MASK) lives in one place.
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

// giphy_is_giphy_channel() removed — use play_scheduler_is_giphy_channel() or type enum instead

// ============================================================================
// Download
// ============================================================================

/**
 * @brief Build download URL for a Giphy channel entry
 *
 * Checks entry->reserved[0] to select the rendition:
 * - 0: uses configured rendition/format
 * - 1: downsized_medium via its dedicated giphy-downsized-medium.gif
 * - 2: downsized_medium passthrough via giphy.gif (the API's rendition url
 *      pointed at the original; no dedicated downsized-medium file exists)
 *
 * This is the only public URL builder: every download must use an
 * entry-aware URL so the per-entry downsized_medium override is honored.
 *
 * @param entry Giphy channel entry
 * @param out_url Output buffer for the full URL
 * @param out_len Output buffer length
 * @return ESP_OK on success
 */
esp_err_t giphy_build_download_url_for_entry(const giphy_channel_entry_t *entry,
                                              char *out_url, size_t out_len);

/**
 * @brief Download a Giphy artwork to the giphy/ folder on SD card
 *
 * Downloads to a temp file, then atomically renames.
 * Creates sharded directories as needed.
 *
 * @param giphy_id Giphy string ID
 * @param url Download URL (build with giphy_build_download_url_for_entry()
 *            so the per-entry rendition override is honored)
 * @param extension Extension index (0=webp, 1=gif)
 * @param out_path Output buffer for the final file path
 * @param out_len Output buffer length
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND on 404,
 *         ESP_FAIL on other errors
 */
esp_err_t giphy_download_artwork(const char *giphy_id, const char *url, uint8_t extension,
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
esp_err_t giphy_download_artwork_with_progress(const char *giphy_id, const char *url,
                                               uint8_t extension,
                                               char *out_path, size_t out_len,
                                               giphy_download_progress_cb_t progress_cb,
                                               void *progress_ctx);

// ============================================================================
// Refresh (called from play_scheduler_refresh)
// ============================================================================

/**
 * @brief Refresh a Giphy channel by fetching GIFs and merging into cache
 *
 * Dispatches to the trending or search endpoint based on the query parameter.
 *
 * Called from play_scheduler_refresh.c when a Giphy channel has
 * refresh_pending = true.
 *
 * @param channel_id    The channel ID (e.g., "giphy_trending" or "giphy_search_cats")
 * @param query         Search query string, or NULL/empty for trending mode
 * @param channel_offset Per-playset starting offset into the Giphy feed.
 *                       Modulo against the 499-entry public cap wraps oversized
 *                       values back to the start.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if WiFi not ready
 */
esp_err_t giphy_refresh_channel(const char *channel_id, const char *query, uint32_t channel_offset);

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
 *
 * @param channel_id     The channel ID
 * @param query          Search query string, or NULL/empty for trending mode
 * @param channel_offset Per-playset starting offset into the Giphy feed
 * @param progress_cb    Callback invoked after each page merge
 * @param progress_ctx   User context for callback
 */
esp_err_t giphy_refresh_channel_with_progress(const char *channel_id,
                                               const char *query,
                                               uint32_t channel_offset,
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

// ============================================================================
// Refresh Status
// ============================================================================

/**
 * @brief Last refresh outcome for display on info screen
 */
typedef enum {
    GIPHY_REFRESH_NOT_ATTEMPTED,
    GIPHY_REFRESH_OK,
    GIPHY_REFRESH_NO_API_KEY,
    GIPHY_REFRESH_INVALID_API_KEY,
    GIPHY_REFRESH_FAILED,
} giphy_refresh_status_t;

/**
 * @brief Get the result of the most recent Giphy refresh
 */
giphy_refresh_status_t giphy_get_last_refresh_status(void);

// ============================================================================
// Rate-limit cooldown (process-wide)
// ============================================================================

/**
 * @brief Mark the Giphy API as rate-limited for the next cooldown_sec seconds.
 *
 * A 429 response applies to the API key, not a single request, so any further
 * Giphy API call within the quota window will also fail. While the cooldown
 * is active, callers (refresh, register_click, etc.) should short-circuit
 * before hitting the network. Pass 0 to use the default (1 hour).
 *
 * Repeated calls extend but do not shorten the cooldown.
 */
void giphy_set_rate_limited(uint32_t cooldown_sec);

/**
 * @brief True if the Giphy API is currently in rate-limit cooldown.
 */
bool giphy_is_rate_limited(void);

/**
 * @brief Seconds remaining until the rate-limit cooldown expires (0 if none).
 */
uint32_t giphy_cooldown_remaining_sec(void);

// ============================================================================
// Auth-invalid latch (process-wide)
// ============================================================================

/**
 * @brief Mark the configured Giphy API key as rejected (HTTP 401/403).
 *
 * Unlike the time-windowed 429 cooldown, a 401/403 is deterministic for the
 * key until the key (or its standing at Giphy) changes — retrying on a timer
 * cannot succeed. The latch suspends all Giphy refreshes; it clears on a key
 * save (giphy_clear_auth_invalid), self-clears when the configured key no
 * longer matches the latched fingerprint (giphy_auth_invalid_for_key), is
 * bypassed by the user's force-refresh override, and expires into a slow
 * reprobe after GIPHY_AUTH_REPROBE_SEC as a self-heal safety net.
 *
 * Called from the giphy_api fetch paths where the HTTP status is known.
 *
 * @param api_key The key that was rejected (fingerprinted, not stored)
 */
void giphy_set_auth_invalid(const char *api_key);

/**
 * @brief True while the auth-invalid latch is engaged (reprobe not yet due).
 */
bool giphy_is_auth_invalid(void);

/**
 * @brief Seconds until the latch expires into a reprobe (0 if not latched).
 */
uint32_t giphy_auth_invalid_remaining_sec(void);

/**
 * @brief Clear all key-parking state (key saved, or a refresh succeeded).
 *
 * Clears both the auth-invalid latch and the no-key flag, and re-arms their
 * one-shot UI notifications so a subsequent bad/missing key notifies the
 * user again. No-op (and silent) when nothing is parked.
 */
void giphy_clear_auth_invalid(void);

/**
 * @brief Mark Giphy refreshes as parked because no API key is configured.
 *
 * Same "persistent until config change" class as the auth-invalid latch,
 * minus the network: only a key write can fix it. Cleared by
 * giphy_clear_auth_invalid (key save via PUT /config); expires after
 * GIPHY_AUTH_REPROBE_SEC only to pick up key writes that bypass http_api.
 * Set at refresh dispatch when the configured key reads back empty.
 */
void giphy_set_no_key(void);

/**
 * @brief True while Giphy refreshes are parked on a missing API key.
 */
bool giphy_is_no_key(void);

/**
 * @brief Seconds until the no-key parking expires into a re-check (0 if not parked).
 */
uint32_t giphy_no_key_remaining_sec(void);

/**
 * @brief One-shot UI-notification claim for the current no-key episode.
 *
 * Same contract as giphy_auth_take_notification: true exactly once after the
 * flag engages, re-armed only by giphy_clear_auth_invalid.
 */
bool giphy_no_key_take_notification(void);

/**
 * @brief True if the latch is engaged for exactly this key.
 *
 * When the latch is engaged but @p api_key no longer matches the latched
 * fingerprint (the key was changed by a writer that bypassed PUT /config),
 * the latch self-clears and false is returned so the new key gets probed.
 */
bool giphy_auth_invalid_for_key(const char *api_key);

/**
 * @brief One-shot UI-notification claim for the current failure episode.
 *
 * Returns true exactly once after the latch engages; re-armed only by
 * giphy_clear_auth_invalid (key save / successful refresh), NOT by hourly
 * reprobe failures — so the on-screen error shows once per episode instead
 * of re-covering live artwork on every retry.
 */
bool giphy_auth_take_notification(void);

#ifdef __cplusplus
}
#endif
