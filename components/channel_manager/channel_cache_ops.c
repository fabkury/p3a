// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file channel_cache_ops.c
 * @brief Channel cache hash table entry operations (add, remove, lookup)
 */

#include "channel_cache.h"
#include "channel_cache_internal.h"
#include "freertos/task.h"
#include "makapix_channel_internal.h"
#include "makapix_channel_utils.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

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

// Pair used to sort LAi by the Ci-derived created_at without repeated hash
// lookups during the comparator (one lookup per entry beforehand).
typedef struct {
    int32_t  post_id;
    uint32_t created_at;
} lai_sort_pair_t;

static int compare_lai_sort_pair_desc(const void *a, const void *b)
{
    const lai_sort_pair_t *pa = (const lai_sort_pair_t *)a;
    const lai_sort_pair_t *pb = (const lai_sort_pair_t *)b;
    if (pa->created_at > pb->created_at) return -1;
    if (pa->created_at < pb->created_at) return 1;
    return 0;
}

void lai_sort_by_created_at_desc(channel_cache_t *cache)
{
    if (!cache || !cache->available_post_ids || cache->available_count <= 1) {
        return;
    }

    lai_sort_pair_t *pairs = psram_malloc(cache->available_count * sizeof(lai_sort_pair_t));
    if (!pairs) {
        ESP_LOGW(TAG, "LAi sort skipped: alloc failed for %zu entries",
                 cache->available_count);
        return;
    }

    for (size_t i = 0; i < cache->available_count; i++) {
        int32_t pid = cache->available_post_ids[i];
        pairs[i].post_id = pid;
        pairs[i].created_at = 0;
        ci_post_id_node_t *node;
        HASH_FIND_INT(cache->post_id_hash, &pid, node);
        if (node && node->ci_index < cache->entry_count && cache->entries) {
            pairs[i].created_at = cache->entries[node->ci_index].created_at;
        }
    }

    qsort(pairs, cache->available_count, sizeof(lai_sort_pair_t),
          compare_lai_sort_pair_desc);

    for (size_t i = 0; i < cache->available_count; i++) {
        cache->available_post_ids[i] = pairs[i].post_id;
    }

    free(pairs);
    ESP_LOGD(TAG, "LAi sorted by created_at DESC: %zu entries",
             cache->available_count);
}

// ============================================================================
// LAi Operations
// ============================================================================

/**
 * @brief Look up a post_id's created_at via the Ci hash (caller holds mutex).
 *
 * Returns 0 if the post_id isn't in Ci or the index is out of range — entries
 * with unknown timestamps cluster at the tail of LAi after a sort.
 */
static uint32_t lai_lookup_created_at(const channel_cache_t *cache, int32_t post_id)
{
    ci_post_id_node_t *node;
    HASH_FIND_INT(cache->post_id_hash, &post_id, node);
    if (!node || node->ci_index >= cache->entry_count || !cache->entries) {
        return 0;
    }
    return cache->entries[node->ci_index].created_at;
}

bool lai_add_entry(channel_cache_t *cache, int32_t post_id, int *out_position)
{
    if (out_position) *out_position = -1;
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

    // Find insertion position to keep LAi sorted by Ci.created_at DESC.
    // Linear scan; binary search wouldn't help asymptotically because the
    // memmove below is also O(N). Inserts run at download-completion rate
    // (bandwidth-limited), so this is comfortably under the budget.
    uint32_t new_created_at = lai_lookup_created_at(cache, post_id);
    size_t pos = cache->available_count;  // Default: append at tail (oldest)
    for (size_t i = 0; i < cache->available_count; i++) {
        uint32_t other = lai_lookup_created_at(cache, cache->available_post_ids[i]);
        if (new_created_at > other) {
            pos = i;
            break;
        }
    }

    if (pos < cache->available_count) {
        memmove(&cache->available_post_ids[pos + 1],
                &cache->available_post_ids[pos],
                (cache->available_count - pos) * sizeof(int32_t));
    }
    cache->available_post_ids[pos] = post_id;
    cache->available_count++;

    // Add to hash (use PSRAM to preserve internal/DMA memory)
    lai_post_id_node_t *node = psram_malloc(sizeof(lai_post_id_node_t));
    if (node) {
        node->post_id = post_id;
        HASH_ADD_INT(cache->lai_hash, post_id, node);
    }

    cache->dirty = true;
    if (out_position) *out_position = (int)pos;

    xSemaphoreGive(cache->mutex);

    ESP_LOGD(TAG, "LAi add: post_id=%ld, pos=%zu, count=%zu",
             (long)post_id, pos, cache->available_count);
    return true;
}

bool lai_remove_entry(channel_cache_t *cache, int32_t post_id, int *out_position)
{
    if (out_position) *out_position = -1;
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

    // Shift-preserving remove keeps LAi sorted by Ci.created_at DESC. Entries
    // at positions > removed_pos shift left by one, so an external recency
    // cursor at cursor > removed_pos must decrement to keep referencing the
    // same logical entry. cursor == removed_pos stays put: the slot now holds
    // what was the next-up entry, which is exactly what cursor should point
    // at next.
    if (cache->available_post_ids) {
        for (size_t i = 0; i < cache->available_count; i++) {
            if (cache->available_post_ids[i] == post_id) {
                memmove(&cache->available_post_ids[i],
                        &cache->available_post_ids[i + 1],
                        (cache->available_count - i - 1) * sizeof(int32_t));
                cache->available_count--;
                if (out_position) *out_position = (int)i;
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
