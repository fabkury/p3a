// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "channel_cache.h"
#include "channel_cache_internal.h"
#include "freertos/task.h"
#include "makapix_channel_internal.h"
#include "makapix_channel_utils.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "channel_cache";

// ============================================================================
// Ci Hash Table Management
// ============================================================================

/**
 * @brief Free Ci hash table
 */
void ci_hash_free(channel_cache_t *cache)
{
    if (!cache) return;

    // Free post_id hash
    ci_post_id_node_t *node, *tmp;
    HASH_ITER(hh, cache->post_id_hash, node, tmp) {
        HASH_DEL(cache->post_id_hash, node);
        free(node);
    }
    cache->post_id_hash = NULL;
}

/**
 * @brief Add single entry to Ci hash table
 */
void ci_hash_add_entry(channel_cache_t *cache, uint32_t ci_index)
{
    if (!cache || !cache->entries || ci_index >= cache->entry_count) return;

    const makapix_channel_entry_t *entry = &cache->entries[ci_index];

    // Add to post_id hash (use PSRAM to preserve internal/DMA memory)
    ci_post_id_node_t *pid_node = psram_malloc(sizeof(ci_post_id_node_t));
    if (pid_node) {
        pid_node->post_id = entry->post_id;
        pid_node->ci_index = ci_index;
        HASH_ADD_INT(cache->post_id_hash, post_id, pid_node);
    }
}

/**
 * @brief Rebuild Ci hash table from entries array
 */
void ci_rebuild_hash_tables(channel_cache_t *cache)
{
    if (!cache) return;

    // Free existing hash tables
    ci_hash_free(cache);

    if (!cache->entries || cache->entry_count == 0) return;

    // Build hash tables
    for (size_t i = 0; i < cache->entry_count; i++) {
        ci_hash_add_entry(cache, (uint32_t)i);
    }

    ESP_LOGD(TAG, "Ci hash table rebuilt: %zu entries", cache->entry_count);
}

// ============================================================================
// LAi Hash Table Management
// ============================================================================

/**
 * @brief Free LAi hash table
 */
void lai_hash_free(channel_cache_t *cache)
{
    if (!cache) return;

    lai_post_id_node_t *node, *tmp;
    HASH_ITER(hh, cache->lai_hash, node, tmp) {
        HASH_DEL(cache->lai_hash, node);
        free(node);
    }
    cache->lai_hash = NULL;
}

/**
 * @brief Rebuild LAi hash from available_post_ids array
 */
void lai_rebuild_hash(channel_cache_t *cache)
{
    if (!cache) return;

    // Free existing hash
    lai_hash_free(cache);

    if (!cache->available_post_ids || cache->available_count == 0) return;

    // Build hash from array (use PSRAM to preserve internal/DMA memory)
    for (size_t i = 0; i < cache->available_count; i++) {
        lai_post_id_node_t *node = psram_malloc(sizeof(lai_post_id_node_t));
        if (node) {
            node->post_id = cache->available_post_ids[i];
            HASH_ADD_INT(cache->lai_hash, post_id, node);
        }
    }

    ESP_LOGD(TAG, "LAi hash rebuilt: %zu entries", cache->available_count);
}

// ============================================================================
// LAi Operations
// ============================================================================

bool lai_add_entry(channel_cache_t *cache, int32_t post_id)
{
    if (!cache) {
        return false;
    }

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    // Check membership via hash O(1)
    lai_post_id_node_t *existing;
    HASH_FIND_INT(cache->lai_hash, &post_id, existing);
    if (existing) {
        xSemaphoreGive(cache->mutex);
        return false;  // Already in LAi
    }

    // Ensure we have space
    if (!cache->available_post_ids) {
        size_t alloc_count = (cache->entry_count > 0) ? cache->entry_count : CHANNEL_CACHE_MAX_ENTRIES;
        cache->available_post_ids = psram_malloc(alloc_count * sizeof(int32_t));
        if (!cache->available_post_ids) {
            xSemaphoreGive(cache->mutex);
            return false;
        }
        cache->available_capacity = alloc_count;
    }

    // Grow the array if capacity is exhausted
    if (cache->available_count >= cache->available_capacity) {
        size_t new_capacity = cache->available_capacity + (cache->available_capacity / 2);
        if (new_capacity < cache->available_capacity + 256) {
            new_capacity = cache->available_capacity + 256;  // Grow by at least 256
        }
        int32_t *new_arr = psram_malloc(new_capacity * sizeof(int32_t));
        if (!new_arr) {
            ESP_LOGE(TAG, "LAi grow failed: %zu -> %zu", cache->available_capacity, new_capacity);
            xSemaphoreGive(cache->mutex);
            return false;
        }
        memcpy(new_arr, cache->available_post_ids, cache->available_count * sizeof(int32_t));
        free(cache->available_post_ids);
        cache->available_post_ids = new_arr;
        cache->available_capacity = new_capacity;
        ESP_LOGI(TAG, "LAi array grew to capacity %zu", new_capacity);
    }

    // Add to array
    cache->available_post_ids[cache->available_count++] = post_id;

    // Add to hash (use PSRAM to preserve internal/DMA memory)
    lai_post_id_node_t *node = psram_malloc(sizeof(lai_post_id_node_t));
    if (node) {
        node->post_id = post_id;
        HASH_ADD_INT(cache->lai_hash, post_id, node);
    }

    cache->dirty = true;

    xSemaphoreGive(cache->mutex);

    ESP_LOGD(TAG, "LAi add: post_id=%ld, count=%zu",
             (long)post_id, cache->available_count);
    return true;
}

bool lai_remove_entry(channel_cache_t *cache, int32_t post_id)
{
    if (!cache) {
        return false;
    }

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    // Find in hash O(1)
    lai_post_id_node_t *node;
    HASH_FIND_INT(cache->lai_hash, &post_id, node);
    if (!node) {
        xSemaphoreGive(cache->mutex);
        return false;  // Not found
    }

    // Remove from hash
    HASH_DEL(cache->lai_hash, node);
    free(node);

    // Find and swap-and-pop from array
    if (cache->available_post_ids) {
        for (size_t i = 0; i < cache->available_count; i++) {
            if (cache->available_post_ids[i] == post_id) {
                cache->available_post_ids[i] = cache->available_post_ids[--cache->available_count];
                break;
            }
        }
    }

    cache->dirty = true;

    xSemaphoreGive(cache->mutex);

    ESP_LOGD(TAG, "LAi remove: post_id=%ld, count=%zu",
             (long)post_id, cache->available_count);
    return true;
}

bool lai_contains(const channel_cache_t *cache, int32_t post_id)
{
    if (!cache) {
        return false;
    }

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    lai_post_id_node_t *node;
    HASH_FIND_INT(cache->lai_hash, &post_id, node);

    xSemaphoreGive(cache->mutex);
    return node != NULL;
}

size_t lai_rebuild(channel_cache_t *cache, const char *vault_path)
{
    if (!cache || !cache->entries || cache->entry_count == 0) {
        return 0;
    }

    // Free existing LAi hash
    lai_hash_free(cache);

    // Ensure LAi array is allocated and large enough
    if (!cache->available_post_ids || cache->available_capacity < cache->entry_count) {
        int32_t *new_arr = psram_malloc(cache->entry_count * sizeof(int32_t));
        if (!new_arr) {
            return 0;
        }
        free(cache->available_post_ids);  // Safe even if NULL
        cache->available_post_ids = new_arr;
        cache->available_capacity = cache->entry_count;
    }

    cache->available_count = 0;
    size_t checked = 0;
    size_t found = 0;

    // Extension strings WITH leading dot (matches actual vault storage)
    static const char *ext_strings[] = {".webp", ".gif", ".png", ".jpg"};

    // Reusable path buffers - declared outside loop to reduce peak stack usage
    char file_path[256];
    char marker_path[264];

    for (size_t i = 0; i < cache->entry_count; i++) {
        const makapix_channel_entry_t *entry = &cache->entries[i];

        // Skip playlists (they don't have direct files)
        if (entry->kind == MAKAPIX_INDEX_POST_KIND_PLAYLIST) {
            continue;
        }

        // Build vault path for this entry
        // Format: {vault_path}/{sha256[0]:02x}/{sha256[1]:02x}/{sha256[2]:02x}/{storage_key}.{ext}
        // This matches makapix_artwork_download() path format exactly
        char uuid_str[37];
        bytes_to_uuid(entry->storage_key_uuid, uuid_str, sizeof(uuid_str));

        uint8_t sha256[32];
        if (storage_key_sha256(uuid_str, sha256) != ESP_OK) {
            continue;
        }

        // Build 3-level directory path (matches actual download paths)
        char dir1[3], dir2[3], dir3[3];
        snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)sha256[0]);
        snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)sha256[1]);
        snprintf(dir3, sizeof(dir3), "%02x", (unsigned int)sha256[2]);

        int ext_idx = (entry->extension < 4) ? entry->extension : 0;

        snprintf(file_path, sizeof(file_path), "%s/%s/%s/%s/%s%s",
                 vault_path, dir1, dir2, dir3, uuid_str, ext_strings[ext_idx]);

        checked++;

        // Check if file exists
        struct stat st;
        if (stat(file_path, &st) == 0 && S_ISREG(st.st_mode)) {
            // Also check for .404 marker
            snprintf(marker_path, sizeof(marker_path), "%s.404", file_path);
            if (stat(marker_path, &st) != 0) {
                // File exists and no 404 marker - store post_id (not ci_index)
                cache->available_post_ids[cache->available_count++] = entry->post_id;
                found++;
            }
        }

        // Yield periodically to avoid watchdog
        if (checked % 100 == 0) {
            taskYIELD();
        }
    }

    // Build LAi hash from array
    lai_rebuild_hash(cache);

    cache->dirty = true;
    ESP_LOGI(TAG, "LAi rebuild: checked %zu, found %zu available", checked, found);
    return found;
}

// ============================================================================
// Ci Operations
// ============================================================================

uint32_t ci_find_by_post_id(const channel_cache_t *cache, int32_t post_id)
{
    if (!cache) {
        return UINT32_MAX;
    }

    // Use hash table for O(1) lookup
    ci_post_id_node_t *node;
    HASH_FIND_INT(cache->post_id_hash, &post_id, node);
    return node ? node->ci_index : UINT32_MAX;
}

const makapix_channel_entry_t *ci_get_entry(const channel_cache_t *cache, uint32_t ci_index)
{
    if (!cache || !cache->entries || ci_index >= cache->entry_count) {
        return NULL;
    }
    return &cache->entries[ci_index];
}

// ============================================================================
// Query Functions
// ============================================================================

esp_err_t channel_cache_get_next_missing(channel_cache_t *cache,
                                         uint32_t *cursor,
                                         makapix_channel_entry_t *out_entry)
{
    if (!cache || !cursor || !out_entry) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    // Iterate from cursor through entries
    while (*cursor < cache->entry_count) {
        const makapix_channel_entry_t *entry = &cache->entries[*cursor];
        (*cursor)++;

        // Skip non-artwork entries
        if (entry->kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            ESP_LOGI(TAG, "SKIP post_id=%d: non-artwork (kind=%d)", entry->post_id, entry->kind);
            continue;
        }

        // Check if already in LAi using hash table (O(1) lookup, no mutex issue)
        lai_post_id_node_t *node;
        HASH_FIND_INT(cache->lai_hash, &entry->post_id, node);
        if (node) {
            continue;  // Already downloaded
        }

        // Found a missing entry - copy it out
        memcpy(out_entry, entry, sizeof(*out_entry));
        xSemaphoreGive(cache->mutex);
        return ESP_OK;
    }

    xSemaphoreGive(cache->mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t channel_cache_get_missing_batch(channel_cache_t *cache,
                                          uint32_t *cursor,
                                          uint32_t max_cursor,
                                          makapix_channel_entry_t *out_entries,
                                          size_t max_batch,
                                          size_t *out_count)
{
    if (!cache || !cursor || !out_entries || !out_count || max_batch == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0;
    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    // Use max_cursor if provided and valid, otherwise entry_count
    uint32_t scan_limit = (max_cursor > 0 && max_cursor < cache->entry_count)
                          ? max_cursor
                          : cache->entry_count;

    // Scan entries and collect up to max_batch missing ones
    while (*cursor < scan_limit && *out_count < max_batch) {
        const makapix_channel_entry_t *entry = &cache->entries[*cursor];
        (*cursor)++;

        // Skip non-artwork entries
        if (entry->kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            continue;
        }

        // Check if already in LAi using hash table (O(1) lookup)
        lai_post_id_node_t *node;
        HASH_FIND_INT(cache->lai_hash, &entry->post_id, node);
        if (node) {
            continue;  // Already downloaded
        }

        // Found a missing entry - copy to output array
        memcpy(&out_entries[*out_count], entry, sizeof(makapix_channel_entry_t));
        (*out_count)++;
    }

    xSemaphoreGive(cache->mutex);

    // Yield to allow other tasks (especially animation rendering) to run
    taskYIELD();

    return (*out_count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}
