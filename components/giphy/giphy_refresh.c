// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file giphy_refresh.c
 * @brief Giphy channel refresh - fetches trending and merges into channel cache
 *
 * Called from play_scheduler_refresh.c when a Giphy channel has
 * refresh_pending = true.
 */

#include "giphy.h"
#include "giphy_types.h"
#include "config_store.h"
#include "channel_cache.h"
#include "channel_metadata.h"
#include "makapix_channel_events.h"
#include "sd_path.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "psram_alloc.h"
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

esp_err_t giphy_refresh_channel(const char *channel_id)
{
    if (!channel_id) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Refreshing Giphy channel: %s", channel_id);

    s_refresh_cancel = false;

    // Determine cache size from config
    uint32_t cache_size = config_store_get_giphy_cache_size();
    if (cache_size == 0) cache_size = 256;
    if (cache_size > 4096) cache_size = 4096;

    // Allocate entry buffer in PSRAM
    size_t alloc_size = cache_size * sizeof(giphy_channel_entry_t);
    giphy_channel_entry_t *entries = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
    if (!entries) {
        entries = malloc(alloc_size);
        if (!entries) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes for entries", alloc_size);
            return ESP_ERR_NO_MEM;
        }
    }

    // Fetch trending
    size_t fetched = 0;
    esp_err_t err = giphy_fetch_trending(entries, cache_size, &fetched);

    if (s_refresh_cancel) {
        ESP_LOGI(TAG, "Refresh cancelled after fetch");
        free(entries);
        return ESP_ERR_NOT_ALLOWED;
    }

    if (err != ESP_OK || fetched == 0) {
        ESP_LOGW(TAG, "Fetch failed or returned 0 entries: %s", esp_err_to_name(err));
        free(entries);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetched %zu trending entries, merging into cache", fetched);

    if (s_refresh_cancel) {
        ESP_LOGI(TAG, "Refresh cancelled before cache merge");
        free(entries);
        return ESP_ERR_NOT_ALLOWED;
    }

    // Find channel cache
    channel_cache_t *cache = channel_cache_registry_find(channel_id);
    if (!cache) {
        ESP_LOGW(TAG, "Channel cache not found for '%s'", channel_id);
        free(entries);
        return ESP_ERR_NOT_FOUND;
    }

    // Merge entries into channel cache
    esp_err_t merge_err = giphy_merge_entries(cache, entries, fetched, cache_size);
    free(entries);

    if (merge_err != ESP_OK) {
        ESP_LOGW(TAG, "Merge failed: %s", esp_err_to_name(merge_err));
        return merge_err;
    }

    // Rebuild LAi (check which files are already downloaded)
    size_t available = giphy_lai_rebuild(cache);

    // Reset download cursors and wake download manager to rescan for new entries.
    // Must use rescan() (not just signal_downloads_needed) because the download
    // manager may have already scanned the empty cache and set channel_complete=true
    // before the refresh finished. rescan() clears that flag and wakes the task.
    extern void download_manager_rescan(void);
    download_manager_rescan();

    // Persist channel metadata (last_refresh timestamp)
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

    ESP_LOGI(TAG, "Giphy channel '%s' refresh complete: %zu entries, %zu available",
             channel_id, cache->entry_count, available);

    return ESP_OK;
}
