// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file makapix_single_artwork.c
 * @brief Transient in-memory single-artwork channel implementation
 */

#include "makapix_internal.h"
#include "psram_alloc.h"
#include "mbedtls/sha256.h"
#include "esp_heap_caps.h"

// ---------------------------------------------------------------------------
// Transient in-memory single-artwork channel implementation
// ---------------------------------------------------------------------------

typedef struct {
    struct channel_s base;
    channel_item_ref_t item;
    bool has_item;
    char art_url[256];
} single_artwork_channel_t;

static esp_err_t storage_key_sha256_local(const char *storage_key, uint8_t out_sha256[32])
{
    if (!storage_key || !out_sha256) return ESP_ERR_INVALID_ARG;
    int ret = mbedtls_sha256((const unsigned char *)storage_key, strlen(storage_key), out_sha256, 0);
    if (ret != 0) {
        ESP_LOGE(MAKAPIX_TAG, "SHA256 failed (ret=%d)", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Extension strings for file naming
static const char *s_ext_strings_local[] = { ".webp", ".gif", ".png", ".jpg" };

static int detect_file_type_ext(const char *url)
{
    size_t len = strlen(url);
    // Check longer extensions first (e.g., .jpeg before .jpg), all comparisons are case-insensitive
    if (len >= 5 && strcasecmp(url + len - 5, ".webp") == 0) return 0; // webp
    if (len >= 5 && strcasecmp(url + len - 5, ".jpeg") == 0) return 3; // JPEG (prefer .jpg but accept .jpeg)
    if (len >= 4 && strcasecmp(url + len - 4, ".gif") == 0)  return 1; // gif
    if (len >= 4 && strcasecmp(url + len - 4, ".png") == 0)  return 2; // png
    if (len >= 4 && strcasecmp(url + len - 4, ".jpg") == 0)  return 3; // JPEG (canonical extension)
    return 0;
}

static void build_vault_path_from_storage_key_simple(const char *storage_key, const char *art_url, char *out, size_t out_len)
{
    char vault_base[128];
    if (sd_path_get_vault(vault_base, sizeof(vault_base)) != ESP_OK) {
        snprintf(out, out_len, "%s/vault/%s.webp", SD_PATH_DEFAULT_ROOT, storage_key);
        return;
    }

    uint8_t sha256[32];
    if (storage_key_sha256_local(storage_key, sha256) != ESP_OK) {
        snprintf(out, out_len, "%s/%s%s", vault_base, storage_key, ".webp");
        return;
    }
    char dir1[3], dir2[3], dir3[3];
    snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)sha256[0]);
    snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)sha256[1]);
    snprintf(dir3, sizeof(dir3), "%02x", (unsigned int)sha256[2]);
    // Include file extension for type detection
    int ext_idx = detect_file_type_ext(art_url);
    snprintf(out, out_len, "%s/%s/%s/%s/%s%s", vault_base, dir1, dir2, dir3, storage_key, s_ext_strings_local[ext_idx]);
}

static esp_err_t single_ch_load(channel_handle_t channel)
{
    single_artwork_channel_t *ch = (single_artwork_channel_t *)channel;
    if (!ch) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(ch->item.filepath, &st) != 0) {
        // Not present, attempt download with retries
        const int max_attempts = 3;
        for (int attempt = 1; attempt <= max_attempts; attempt++) {
            ESP_LOGD(MAKAPIX_TAG, "Downloading artwork (attempt %d/%d)...", attempt, max_attempts);
            esp_err_t err = makapix_artwork_download(ch->art_url, ch->item.storage_key, ch->item.filepath, sizeof(ch->item.filepath));
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGW(MAKAPIX_TAG, "Download attempt %d failed: %s", attempt, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (err == ESP_ERR_NOT_FOUND) {
                // Permanent miss (e.g., HTTP 404). Do not retry.
                return ESP_ERR_NOT_FOUND;
            }
            if (attempt == max_attempts) {
                return ESP_FAIL;
            }
        }
    }

    ch->has_item = true;
    channel->loaded = true;
    return ESP_OK;
}

static void single_ch_unload(channel_handle_t channel)
{
    single_artwork_channel_t *ch = (single_artwork_channel_t *)channel;
    if (!ch) return;
    ch->has_item = false;
    channel->loaded = false;
}

static esp_err_t single_ch_start_playback(channel_handle_t channel, channel_order_mode_t order_mode, const channel_filter_config_t *filter)
{
    (void)order_mode;
    (void)filter;
    single_artwork_channel_t *ch = (single_artwork_channel_t *)channel;
    if (!ch || !ch->has_item) return ESP_ERR_NOT_FOUND;
    channel->current_order = CHANNEL_ORDER_ORIGINAL;
    channel->current_filter = filter ? *filter : (channel_filter_config_t){0};
    return ESP_OK;
}

static esp_err_t single_ch_next(channel_handle_t channel, channel_item_ref_t *out_item)
{
    single_artwork_channel_t *ch = (single_artwork_channel_t *)channel;
    if (!ch || !ch->has_item) return ESP_ERR_NOT_FOUND;
    if (out_item) *out_item = ch->item;
    return ESP_OK;
}

static esp_err_t single_ch_prev(channel_handle_t channel, channel_item_ref_t *out_item)
{
    return single_ch_next(channel, out_item);
}

static esp_err_t single_ch_current(channel_handle_t channel, channel_item_ref_t *out_item)
{
    return single_ch_next(channel, out_item);
}

static esp_err_t single_ch_request_reshuffle(channel_handle_t channel)
{
    (void)channel;
    return ESP_OK;
}

static esp_err_t single_ch_request_refresh(channel_handle_t channel)
{
    (void)channel;
    return ESP_OK;
}

static esp_err_t single_ch_get_stats(channel_handle_t channel, channel_stats_t *out_stats)
{
    single_artwork_channel_t *ch = (single_artwork_channel_t *)channel;
    if (!out_stats) return ESP_ERR_INVALID_ARG;
    out_stats->total_items = ch->has_item ? 1 : 0;
    out_stats->filtered_items = out_stats->total_items;
    out_stats->current_position = ch->has_item ? 0 : 0;
    return ESP_OK;
}

static void single_ch_destroy(channel_handle_t channel)
{
    single_artwork_channel_t *ch = (single_artwork_channel_t *)channel;
    if (!ch) return;
    if (ch->base.name) free(ch->base.name);
    free(ch);
}

static const channel_ops_t s_single_ops = {
    .load = single_ch_load,
    .unload = single_ch_unload,
    .start_playback = single_ch_start_playback,
    .next_item = single_ch_next,
    .prev_item = single_ch_prev,
    .current_item = single_ch_current,
    .request_reshuffle = single_ch_request_reshuffle,
    .request_refresh = single_ch_request_refresh,
    .get_stats = single_ch_get_stats,
    .destroy = single_ch_destroy,
};

static __attribute__((unused)) channel_handle_t create_single_artwork_channel(const char *storage_key, const char *art_url)
{
    single_artwork_channel_t *ch = psram_calloc(1, sizeof(single_artwork_channel_t));
    if (!ch) return NULL;

    ch->base.ops = &s_single_ops;
    ch->base.loaded = false;
    ch->base.current_order = CHANNEL_ORDER_ORIGINAL;
    ch->base.current_filter.required_flags = CHANNEL_FILTER_FLAG_NONE;
    ch->base.current_filter.excluded_flags = CHANNEL_FILTER_FLAG_NONE;
    ch->base.name = psram_strdup("Artwork");

    // Build filepath from storage_key (includes extension from art_url)
    build_vault_path_from_storage_key_simple(storage_key, art_url, ch->item.filepath, sizeof(ch->item.filepath));
    strncpy(ch->item.storage_key, storage_key, sizeof(ch->item.storage_key) - 1);
    ch->item.storage_key[sizeof(ch->item.storage_key) - 1] = '\0';
    ch->item.item_index = 0;
    // Set format flags
    switch (detect_file_type_ext(art_url)) {
        case 1: ch->item.flags = CHANNEL_FILTER_FLAG_GIF; break;
        case 2: ch->item.flags = CHANNEL_FILTER_FLAG_PNG; break;
        case 3: ch->item.flags = CHANNEL_FILTER_FLAG_JPEG; break;
        case 0:
        default: ch->item.flags = CHANNEL_FILTER_FLAG_WEBP; break;
    }
    strncpy(ch->art_url, art_url, sizeof(ch->art_url) - 1);
    ch->art_url[sizeof(ch->art_url) - 1] = '\0';
    ch->has_item = false;
    return (channel_handle_t)ch;
}
