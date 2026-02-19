// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file giphy_refresh.c
 * @brief Giphy channel refresh - fetches trending/search and merges into cache
 *
 * Called from play_scheduler_refresh.c when a Giphy channel has
 * refresh_pending = true. Dispatches to the trending or search endpoint
 * based on channel_id prefix.
 */

#include "giphy.h"
#include "giphy_types.h"
#include "config_store.h"
#include "channel_cache.h"
#include "channel_metadata.h"
#include "sntp_sync.h"
#include "makapix_channel_events.h"
#include "sd_path.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "psram_alloc.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "giphy_refresh";

static volatile bool s_refresh_cancel = false;

void giphy_cancel_refresh(void)
{
    s_refresh_cancel = true;
    ESP_LOGI(TAG, "Giphy refresh cancellation requested");
}

bool giphy_is_refresh_cancelled(void)
{
    return s_refresh_cancel;
}

/**
 * @brief Merge Giphy entries into a channel cache
 *
 * Unlike channel_cache_merge_posts() (which takes makapix_post_t from MQTT),
 * this works directly with giphy_channel_entry_t which are already in the
 * 64-byte packed format used by channel_cache_t.
 *
 * Deduplicates by post_id. New entries are appended, existing entries updated.
 * Rebuilds hash tables after merge.
 */
static esp_err_t giphy_merge_entries(channel_cache_t *cache,
                                     const giphy_channel_entry_t *new_entries,
                                     size_t new_count,
                                     size_t max_entries)
{
    if (!cache || !new_entries || new_count == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    // Allocate combined array
    size_t max_total = cache->entry_count + new_count;
    if (max_total > max_entries) max_total = max_entries;

    makapix_channel_entry_t *all = psram_malloc(max_total * sizeof(makapix_channel_entry_t));
    if (!all) {
        xSemaphoreGive(cache->mutex);
        return ESP_ERR_NO_MEM;
    }

    // Copy existing entries
    size_t all_count = 0;
    if (cache->entries && cache->entry_count > 0) {
        size_t copy_count = cache->entry_count;
        if (copy_count > max_total) copy_count = max_total;
        memcpy(all, cache->entries, copy_count * sizeof(makapix_channel_entry_t));
        all_count = copy_count;
    }

    // Merge new entries (dedup by post_id)
    for (size_t i = 0; i < new_count && all_count < max_total; i++) {
        const giphy_channel_entry_t *ne = &new_entries[i];

        // Check if already in cache
        bool found = false;
        for (size_t j = 0; j < all_count; j++) {
            // post_id is at offset 0 for both structs
            if (all[j].post_id == ne->post_id) {
                // Update in place
                memcpy(&all[j], ne, sizeof(makapix_channel_entry_t));
                found = true;
                break;
            }
        }

        if (!found) {
            // Append new entry (reinterpret as makapix_channel_entry_t since same size)
            memcpy(&all[all_count], ne, sizeof(makapix_channel_entry_t));
            all_count++;
        }
    }

    // Replace cache entries
    if (cache->entries) {
        free(cache->entries);
    }
    cache->entries = all;
    cache->entry_count = all_count;

    // Rebuild Ci hash table
    ci_post_id_node_t *node, *tmp;
    HASH_ITER(hh, cache->post_id_hash, node, tmp) {
        HASH_DEL(cache->post_id_hash, node);
        free(node);
    }
    cache->post_id_hash = NULL;

    for (size_t i = 0; i < all_count; i++) {
        ci_post_id_node_t *n = psram_malloc(sizeof(ci_post_id_node_t));
        if (n) {
            n->post_id = all[i].post_id;
            n->ci_index = (uint32_t)i;
            HASH_ADD_INT(cache->post_id_hash, post_id, n);
        }
    }

    cache->dirty = true;

    xSemaphoreGive(cache->mutex);

    // Save cache
    char channels_path[128];
    if (sd_path_get_channel(channels_path, sizeof(channels_path)) != ESP_OK) {
        strlcpy(channels_path, "/sdcard/p3a/channel", sizeof(channels_path));
    }
    channel_cache_save(cache, channels_path);

    return ESP_OK;
}

/**
 * @brief Rebuild LAi for Giphy channel by checking file existence
 *
 * Similar to lai_rebuild() but uses giphy_build_filepath instead of vault paths.
 */
static size_t giphy_lai_rebuild(channel_cache_t *cache)
{
    if (!cache) return 0;

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    // Clear existing LAi
    lai_post_id_node_t *node, *tmp;
    HASH_ITER(hh, cache->lai_hash, node, tmp) {
        HASH_DEL(cache->lai_hash, node);
        free(node);
    }
    cache->lai_hash = NULL;

    if (cache->available_post_ids) {
        free(cache->available_post_ids);
        cache->available_post_ids = NULL;
    }
    cache->available_count = 0;
    cache->available_capacity = 0;

    if (!cache->entries || cache->entry_count == 0) {
        xSemaphoreGive(cache->mutex);
        return 0;
    }

    // Allocate LAi array
    cache->available_post_ids = psram_malloc(cache->entry_count * sizeof(int32_t));
    if (!cache->available_post_ids) {
        xSemaphoreGive(cache->mutex);
        return 0;
    }
    cache->available_capacity = cache->entry_count;

    // Check each entry for file existence
    size_t found = 0;
    for (size_t i = 0; i < cache->entry_count; i++) {
        const giphy_channel_entry_t *ge = (const giphy_channel_entry_t *)&cache->entries[i];

        char filepath[256];
        giphy_build_filepath(ge->giphy_id, ge->extension, filepath, sizeof(filepath));

        struct stat st;
        if (stat(filepath, &st) == 0 && st.st_size > 0) {
            cache->available_post_ids[found] = ge->post_id;

            // Add to hash set
            lai_post_id_node_t *n = psram_malloc(sizeof(lai_post_id_node_t));
            if (n) {
                n->post_id = ge->post_id;
                HASH_ADD_INT(cache->lai_hash, post_id, n);
            }

            found++;
        }
    }

    cache->available_count = found;
    cache->dirty = true;

    xSemaphoreGive(cache->mutex);

    return found;
}

// Response buffer size for Giphy API (allocated in PSRAM).
// Each GIF object is ~8KB of JSON; at 25 items/page this is ~200KB.
#define GIPHY_RESPONSE_BUF_SIZE (256 * 1024)

esp_err_t giphy_refresh_channel(const char *channel_id)
{
    if (!channel_id) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Refreshing Giphy channel: %s", channel_id);

    s_refresh_cancel = false;

    // Find channel cache early — fail fast before allocating buffers
    channel_cache_t *cache = channel_cache_registry_find(channel_id);
    if (!cache) {
        ESP_LOGW(TAG, "Channel cache not found for '%s'", channel_id);
        return ESP_ERR_NOT_FOUND;
    }

    // Determine cache size from config
    uint32_t cache_size = config_store_get_giphy_cache_size();
    if (cache_size == 0) cache_size = 256;
    if (cache_size > 4096) cache_size = 4096;

    // Read API config into fetch context
    giphy_fetch_ctx_t ctx = {0};
    config_store_get_giphy_api_key(ctx.api_key, sizeof(ctx.api_key));
    if (ctx.api_key[0] == '\0') {
        ESP_LOGE(TAG, "No Giphy API key configured");
        return ESP_ERR_NOT_FOUND;
    }
    if (config_store_get_giphy_rendition(ctx.rendition, sizeof(ctx.rendition)) != ESP_OK) {
        strlcpy(ctx.rendition, CONFIG_GIPHY_RENDITION_DEFAULT, sizeof(ctx.rendition));
    }
    if (config_store_get_giphy_format(ctx.format, sizeof(ctx.format)) != ESP_OK) {
        strlcpy(ctx.format, CONFIG_GIPHY_FORMAT_DEFAULT, sizeof(ctx.format));
    }
    if (config_store_get_giphy_rating(ctx.rating, sizeof(ctx.rating)) != ESP_OK) {
        strlcpy(ctx.rating, "pg-13", sizeof(ctx.rating));
    }

    // Parse channel_id to determine trending vs search mode.
    // Channel IDs: "giphy_trending" -> trending, "giphy_search_cats" -> search "cats"
    const char *after_prefix = channel_id + 6;  // skip "giphy_"
    if (strncmp(after_prefix, "search_", 7) == 0 && after_prefix[7] != '\0') {
        strlcpy(ctx.query, after_prefix + 7, sizeof(ctx.query));
        for (char *p = ctx.query; *p; p++) {
            if (*p == '_') *p = ' ';
        }
        ESP_LOGI(TAG, "Search mode: q=\"%s\"", ctx.query);
    } else {
        ctx.query[0] = '\0';
    }

    // Allocate response buffer in PSRAM (shared across all pages)
    ctx.response_buf_size = GIPHY_RESPONSE_BUF_SIZE;
    ctx.response_buf = heap_caps_malloc(ctx.response_buf_size, MALLOC_CAP_SPIRAM);
    if (!ctx.response_buf) {
        ctx.response_buf = malloc(ctx.response_buf_size);
        if (!ctx.response_buf) {
            ESP_LOGE(TAG, "Failed to allocate response buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    // Rebuild LAi BEFORE the loop so the download manager recognizes
    // files already on disk from previous sessions
    size_t available = giphy_lai_rebuild(cache);
    ESP_LOGI(TAG, "LAi rebuilt: %zu files already available", available);

    extern void download_manager_rescan(void);

    // Per-page entry buffer on the stack (25 entries x 64 bytes = 1600 bytes)
    giphy_channel_entry_t page_entries[25];
    size_t total_fetched = 0;
    int offset = 0;
    esp_err_t last_err = ESP_OK;
    bool refresh_completed = true;

    while ((size_t)offset < cache_size) {
        if (s_refresh_cancel) {
            ESP_LOGI(TAG, "Refresh cancelled before page fetch (offset=%d)", offset);
            refresh_completed = false;
            break;
        }

        size_t page_count = 0;
        bool has_more = false;
        esp_err_t err = giphy_fetch_page(&ctx, offset, page_entries,
                                                  &page_count, &has_more);

        if (s_refresh_cancel) {
            ESP_LOGI(TAG, "Refresh cancelled after page fetch (offset=%d)", offset);
            refresh_completed = false;
            break;
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Page fetch failed at offset=%d: %s", offset, esp_err_to_name(err));
            last_err = err;
            refresh_completed = false;
            break;
        }

        if (page_count == 0) {
            ESP_LOGI(TAG, "No entries returned at offset=%d, done", offset);
            break;
        }

        // Merge this page into cache
        esp_err_t merge_err = giphy_merge_entries(cache, page_entries, page_count, cache_size);
        if (merge_err != ESP_OK) {
            ESP_LOGW(TAG, "Merge failed at offset=%d: %s", offset, esp_err_to_name(merge_err));
            refresh_completed = false;
            break;
        }

        total_fetched += page_count;
        ESP_LOGI(TAG, "Page merged: %zu entries (total: %zu)", page_count, total_fetched);

        // Signal download manager — downloads can start while we fetch more pages
        download_manager_rescan();

        offset += (int)page_count;

        if (!has_more) {
            break;
        }

        // Brief delay between pages to be nice to the API
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(ctx.response_buf);

    // Only persist last_refresh timestamp when the refresh ran to completion.
    // Cancelled or failed refreshes must not update the timestamp, otherwise
    // the next refresh attempt will consider the channel "still fresh".
    // Only save when the clock is synchronized — an unsynchronized clock would
    // write a garbage timestamp that poisons future cooldown checks.
    if (refresh_completed && sntp_sync_is_synchronized()) {
        char channels_path[128];
        if (sd_path_get_channel(channels_path, sizeof(channels_path)) != ESP_OK) {
            strlcpy(channels_path, "/sdcard/p3a/channel", sizeof(channels_path));
        }
        channel_metadata_t meta = {
            .last_refresh = time(NULL),
            .cursor = "",
        };
        esp_err_t meta_err = channel_metadata_save(channel_id, channels_path, &meta);
        if (meta_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save channel metadata: %s", esp_err_to_name(meta_err));
        }
    } else if (refresh_completed) {
        ESP_LOGI(TAG, "Clock not synchronized, deferring metadata save for '%s'",
                 channel_id);
    }

    ESP_LOGI(TAG, "Giphy channel '%s' refresh %s: %zu fetched, %zu in cache",
             channel_id, refresh_completed ? "complete" : "incomplete",
             total_fetched, cache->entry_count);

    if (total_fetched > 0) return ESP_OK;
    // Propagate specific error (e.g. ESP_ERR_NOT_ALLOWED for invalid API key)
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}
