// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file klipy.h
 * @brief Klipy component public API: init, trending/search, download, cache helpers
 *
 * Klipy (klipy.com) is a Giphy/Tenor-style GIF + Sticker API. This component
 * mirrors components/giphy/ almost 1:1; the two meaningful differences are:
 *   1. Download URLs are opaque per-format CDN tokens that cannot be
 *      reconstructed, so we store the numeric id and re-resolve at download.
 *   2. A rejected API key is signalled as HTTP 404 on a valid route (not
 *      401/403), so the auth-invalid latch engages on 404 in klipy_fetch_page.
 */

#pragma once

#include "klipy_types.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/// Items requested per Klipy API page.
#define KLIPY_PAGE_LIMIT 50

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Initialization
// ============================================================================

esp_err_t klipy_init(void);
void klipy_deinit(void);

// ============================================================================
// API Client
// ============================================================================

/**
 * @brief Context for Klipy API fetch operations
 */
typedef struct {
    char api_key[128];
    char product[12];           ///< "gifs" or "stickers" (API endpoint prefix)
    char query[64];             ///< empty = trending; non-empty = search/category query
    char rating[8];             ///< "g" | "pg" | "pg-13" | "r"
    char format[8];             ///< "gif" or "webp" (preferred download format)
    char *response_buf;         ///< caller-allocated buffer (PSRAM recommended)
    size_t response_buf_size;   ///< size of response_buf in bytes
    uint16_t screen_width;      ///< display width (for rendition selection)
    uint16_t screen_height;     ///< display height (for rendition selection)
    uint8_t product_id;         ///< KLIPY_PRODUCT_GIF / KLIPY_PRODUCT_STICKER (stored in entries)
} klipy_fetch_ctx_t;

/**
 * @brief Fetch a single page of items from Klipy (trending or search)
 *
 * When ctx->query is empty, fetches {product}/trending; otherwise
 * {product}/search?q=. Klipy pagination is 1-based (page >= 1).
 *
 * @param ctx          Fetch context (API config + shared response buffer)
 * @param page         1-based page number
 * @param out_entries  Output array (must hold at least KLIPY_PAGE_LIMIT entries)
 * @param out_count    Number of entries parsed from this page
 * @param out_has_more Set from the response `has_next` flag
 * @return ESP_OK on success; ESP_ERR_NOT_ALLOWED on a rejected key (HTTP 404 on
 *         a valid route, or 401/403); ESP_ERR_INVALID_RESPONSE on HTTP 429;
 *         ESP_FAIL on other errors.
 */
esp_err_t klipy_fetch_page(klipy_fetch_ctx_t *ctx, int page,
                           klipy_channel_entry_t *out_entries,
                           size_t *out_count, bool *out_has_more);

// ============================================================================
// Cache / Path Helpers
// ============================================================================

/**
 * @brief Build filepath for a Klipy artwork on SD card
 *
 * Path format: /sdcard/p3a/klipy/{gif|sticker}/{d0}/{d1}/{klipy_id}.{ext}
 * Built via the shared sd_path_build_sharded() helper.
 */
esp_err_t klipy_build_filepath(uint64_t klipy_id, uint8_t product, uint8_t extension,
                               char *out_path, size_t out_len);

/**
 * @brief Build filepath for a klipy_channel_entry_t
 */
void klipy_build_entry_filepath(const klipy_channel_entry_t *entry,
                                char *out_path, size_t out_len);

/**
 * @brief Fold a numeric Klipy id into an int32_t post_id (never 0)
 */
int32_t klipy_id_to_post_id(uint64_t klipy_id);

// ============================================================================
// Download
// ============================================================================

typedef void (*klipy_download_progress_cb_t)(size_t bytes_read, size_t content_length, void *user_ctx);

/**
 * @brief Download a Klipy artwork to the klipy/ folder on SD card
 *
 * Re-resolves the opaque CDN url via GET {product}/{id}, then downloads it to a
 * temp file and atomically renames. Creates sharded directories as needed.
 *
 * @param klipy_id   Numeric Klipy id
 * @param product    KLIPY_PRODUCT_GIF / KLIPY_PRODUCT_STICKER
 * @param extension  Extension index (0=webp, 1=gif) — selects the format to resolve
 * @param out_path   Output buffer for the final file path
 * @param out_len    Output buffer length
 */
esp_err_t klipy_download_artwork(uint64_t klipy_id, uint8_t product, uint8_t extension,
                                 char *out_path, size_t out_len);

esp_err_t klipy_download_artwork_with_progress(uint64_t klipy_id, uint8_t product, uint8_t extension,
                                               char *out_path, size_t out_len,
                                               klipy_download_progress_cb_t progress_cb,
                                               void *progress_ctx);

// ============================================================================
// Refresh (called from play_scheduler_refresh)
// ============================================================================

/**
 * @brief Refresh a Klipy channel by fetching pages and merging into cache
 *
 * @param channel_id     The channel ID
 * @param spec_name      "{product}:{mode}" — product in {gif,sticker}, mode in
 *                       {trending,search,category}. category is served via the
 *                       search endpoint using the category's query.
 * @param identifier     Search/category query, or NULL/empty for trending
 * @param channel_offset Per-playset starting offset into the feed (wrapped
 *                       modulo KLIPY_OFFSET_CAP)
 * @return ESP_OK only when the refresh ran to completion (see giphy_refresh for
 *         the partial-refresh contract this mirrors).
 */
esp_err_t klipy_refresh_channel(const char *channel_id, const char *spec_name,
                                const char *identifier, uint32_t channel_offset);

typedef void (*klipy_refresh_progress_cb_t)(int current, int total, void *user_ctx);

esp_err_t klipy_refresh_channel_with_progress(const char *channel_id, const char *spec_name,
                                              const char *identifier, uint32_t channel_offset,
                                              klipy_refresh_progress_cb_t progress_cb,
                                              void *progress_ctx);

void klipy_cancel_refresh(void);
bool klipy_is_refresh_cancelled(void);

// ============================================================================
// Refresh Status
// ============================================================================

typedef enum {
    KLIPY_REFRESH_NOT_ATTEMPTED,
    KLIPY_REFRESH_OK,
    KLIPY_REFRESH_NO_API_KEY,
    KLIPY_REFRESH_INVALID_API_KEY,
    KLIPY_REFRESH_FAILED,
} klipy_refresh_status_t;

klipy_refresh_status_t klipy_get_last_refresh_status(void);

// ============================================================================
// Rate-limit cooldown (process-wide) — identical semantics to Giphy
// ============================================================================

void klipy_set_rate_limited(uint32_t cooldown_sec);
bool klipy_is_rate_limited(void);
uint32_t klipy_cooldown_remaining_sec(void);

// ============================================================================
// Auth-invalid latch + no-key parking (process-wide) — identical to Giphy
// ============================================================================

void klipy_set_auth_invalid(const char *api_key);
bool klipy_is_auth_invalid(void);
uint32_t klipy_auth_invalid_remaining_sec(void);
void klipy_clear_auth_invalid(void);
void klipy_set_no_key(void);
bool klipy_is_no_key(void);
uint32_t klipy_no_key_remaining_sec(void);
bool klipy_no_key_take_notification(void);
bool klipy_auth_invalid_for_key(const char *api_key);
bool klipy_auth_take_notification(void);

#ifdef __cplusplus
}
#endif
