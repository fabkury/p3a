// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_lai.c
 * @brief Play Scheduler - LAi (Locally Available index) operations
 *
 * This file implements:
 * - LAi management (add/remove entries)
 * - Download completion callbacks
 * - Load failure handling
 * - Download manager integration APIs
 */

#include "play_scheduler.h"
#include "play_scheduler_internal.h"
#include "channel_cache.h"
#include "makapix_channel_utils.h"
#include "event_bus.h"
#include "esp_log.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "ps_lai";

// ============================================================================
// Internal Helper Functions
// ============================================================================

/**
 * @brief Find channel index by channel_id
 */
static int ps_find_channel_index(ps_state_t *state, const char *channel_id)
{
    if (!channel_id) return -1;

    for (size_t i = 0; i < state->channel_count; i++) {
        if (strcmp(state->channels[i].channel_id, channel_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Check if a post_id is already in LAi
 */
static bool ps_lai_contains(ps_channel_state_t *ch, int32_t post_id)
{
    // Makapix channels: use cache directly (cache may reallocate during merges)
    if (ch->cache) {
        int32_t *available_post_ids = ch->cache->available_post_ids;
        size_t available_count = ch->cache->available_count;
        if (!available_post_ids) return false;
        for (size_t i = 0; i < available_count; i++) {
            if (available_post_ids[i] == post_id) {
                return true;
            }
        }
        return false;
    }

    // SD card channels: use direct fields
    if (!ch->available_post_ids) return false;

    for (size_t i = 0; i < ch->available_count; i++) {
        if (ch->available_post_ids[i] == post_id) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Add an entry to LAi by ci_index
 *
 * For Makapix channels, delegates to channel_cache module which handles
 * dirty tracking and debounced persistence. The cache API now uses post_id.
 *
 * Thread-safety: Takes cache mutex to validate ci_index and get post_id,
 * as the cache may be reallocated during merge operations.
 */
static bool ps_lai_add(ps_channel_state_t *ch, uint32_t ci_index)
{
    // For Makapix channels with cache, use channel_cache module
    if (ch->cache) {
        // Take mutex to safely validate ci_index and get post_id
        // (cache entries may be reallocated during merge operations)
        xSemaphoreTake(ch->cache->mutex, portMAX_DELAY);

        // Validate ci_index against current cache size (not ch->entry_count which may be stale)
        if (ci_index >= ch->cache->entry_count) {
            xSemaphoreGive(ch->cache->mutex);
            return false;
        }

        // Get post_id under mutex
        int32_t post_id = ch->cache->entries[ci_index].post_id;
        xSemaphoreGive(ch->cache->mutex);

        // lai_add_entry takes its own mutex, so we release ours first
        bool added = lai_add_entry(ch->cache, post_id);
        if (added) {
            ch->active = true;
            channel_cache_schedule_save(ch->cache);
        }
        return added;
    }

    // Fallback for SD card channels (shouldn't have LAi, but keep for safety)
    if (ci_index >= ch->entry_count) return false;

    makapix_channel_entry_t *entries = (makapix_channel_entry_t *)ch->entries;
    int32_t post_id = entries[ci_index].post_id;
    if (ps_lai_contains(ch, post_id)) return false;

    if (!ch->available_post_ids) {
        ch->available_post_ids = malloc(ch->entry_count * sizeof(int32_t));
        if (!ch->available_post_ids) return false;
        ch->available_count = 0;
    }

    ch->available_post_ids[ch->available_count++] = post_id;
    ch->active = true;
    return true;
}

/**
 * @brief Remove an entry from LAi by ci_index (swap-and-pop for O(1))
 *
 * For Makapix channels, delegates to channel_cache module which handles
 * dirty tracking and debounced persistence. The cache API now uses post_id.
 *
 * Thread-safety: Takes cache mutex to validate ci_index and get post_id,
 * as the cache may be reallocated during merge operations.
 */
static bool ps_lai_remove(ps_channel_state_t *ch, uint32_t ci_index)
{
    // For Makapix channels with cache, use channel_cache module
    if (ch->cache) {
        // Take mutex to safely validate ci_index and get post_id
        xSemaphoreTake(ch->cache->mutex, portMAX_DELAY);

        // Validate ci_index against current cache size
        if (ci_index >= ch->cache->entry_count) {
            xSemaphoreGive(ch->cache->mutex);
            return false;
        }

        // Get post_id under mutex
        int32_t post_id = ch->cache->entries[ci_index].post_id;
        xSemaphoreGive(ch->cache->mutex);

        // lai_remove_entry takes its own mutex
        bool removed = lai_remove_entry(ch->cache, post_id);
        if (removed) {
            channel_cache_schedule_save(ch->cache);
        }
        return removed;
    }

    // Fallback for SD card channels
    if (!ch->available_post_ids || ch->available_count == 0) return false;
    if (ci_index >= ch->entry_count) return false;

    makapix_channel_entry_t *entries = (makapix_channel_entry_t *)ch->entries;
    int32_t post_id = entries[ci_index].post_id;
    for (size_t i = 0; i < ch->available_count; i++) {
        if (ch->available_post_ids[i] == post_id) {
            ch->available_post_ids[i] = ch->available_post_ids[ch->available_count - 1];
            ch->available_count--;
            return true;
        }
    }
    return false;
}

// ============================================================================
// Download Completion Callback
// ============================================================================

void play_scheduler_on_download_complete(const char *channel_id, int32_t post_id)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized || !channel_id || post_id == 0) {
        return;
    }

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);

    int ch_idx = ps_find_channel_index(s_state, channel_id);
    if (ch_idx < 0) {
        ESP_LOGD(TAG, "Download complete for unknown channel: %s", channel_id);
        xSemaphoreGive(s_state->mutex);
        return;
    }

    ps_channel_state_t *ch = &s_state->channels[ch_idx];

    // Look up cache from registry to ensure we have current data
    channel_cache_t *cache = channel_cache_registry_find(ch->channel_id);
    if (!cache) {
        cache = ch->cache;
    }

    // Find the Ci index for this post_id using O(1) hash lookup
    uint32_t ci_index = UINT32_MAX;
    if (cache) {
        ci_index = ci_find_by_post_id(cache, post_id);
    }

    if (ci_index == UINT32_MAX) {
        // Entry not in Ci — either evicted during a merge truncation or lost
        // during a concurrent cache reallocation.  The downloaded file is already
        // on disk; it will be picked up into LAi on the next refresh cycle.
        ESP_LOGI(TAG, "Downloaded post_id=%ld not found in Ci for ch='%s', skipping LAi add (file on disk)",
                 (long)post_id, ch->display_name);
        xSemaphoreGive(s_state->mutex);
        return;
    }

    // Track if this is a zero-to-one transition
    size_t prev_total_available = 0;
    for (size_t i = 0; i < s_state->channel_count; i++) {
        ps_channel_state_t *c = &s_state->channels[i];
        prev_total_available += (c->cache ? c->cache->available_count : c->available_count);
    }

    // Add to LAi
    size_t prev_channel_available = ch->cache ? ch->cache->available_count : ch->available_count;
    if (ps_lai_add(ch, ci_index)) {
        size_t new_channel_available = ch->cache ? ch->cache->available_count : ch->available_count;
        size_t ci_count = ch->cache ? ch->cache->entry_count : ch->entry_count;
        ESP_LOGI(TAG, ">>> LAi ADD: ch='%s' post_id=%ld ci=%lu, LAi: %zu -> %zu (Ci=%zu)",
                 ch->display_name, (long)post_id, (unsigned long)ci_index,
                 prev_channel_available, new_channel_available, ci_count);

        // Check for zero-to-one transition
        if (prev_total_available == 0 && new_channel_available > 0 && !s_state->playback_triggered) {
            ESP_LOGI(TAG, "Zero-to-one transition - triggering playback");
            s_state->playback_triggered = true;
            xSemaphoreGive(s_state->mutex);

            // Trigger playback via event bus to avoid race condition
            event_bus_emit_simple(P3A_EVENT_SWAP_NEXT);
            return;
        }
    } else {
        ESP_LOGD(TAG, "LAi add skipped (already present?): ch='%s' post_id=%ld ci=%lu",
                 channel_id, (long)post_id, (unsigned long)ci_index);
    }

    xSemaphoreGive(s_state->mutex);
}

// ============================================================================
// Stats & Availability
// ============================================================================

size_t play_scheduler_get_total_available(void)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized) {
        return 0;
    }

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);

    size_t total = 0;
    for (size_t i = 0; i < s_state->channel_count; i++) {
        ps_channel_state_t *ch = &s_state->channels[i];
        total += (ch->cache ? ch->cache->available_count : ch->available_count);
    }

    xSemaphoreGive(s_state->mutex);

    return total;
}

void play_scheduler_get_channel_stats(const char *channel_id, size_t *out_total, size_t *out_cached)
{
    if (out_total) *out_total = 0;
    if (out_cached) *out_cached = 0;

    ps_state_t *s_state = ps_get_state();

    if (!channel_id || !s_state->initialized) {
        return;
    }

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);

    // Find channel by ID
    for (size_t i = 0; i < s_state->channel_count; i++) {
        ps_channel_state_t *ch = &s_state->channels[i];
        if (strcmp(ch->channel_id, channel_id) == 0) {
            if (out_total) *out_total = (ch->cache ? ch->cache->entry_count : ch->entry_count);
            if (out_cached) *out_cached = (ch->cache ? ch->cache->available_count : ch->available_count);
            break;
        }
    }

    xSemaphoreGive(s_state->mutex);
}

// ============================================================================
// Download Manager Integration
// ============================================================================

size_t play_scheduler_get_channel_entry_count(const char *channel_id)
{
    ps_state_t *s_state = ps_get_state();

    if (!channel_id || !s_state->initialized) {
        return 0;
    }

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);

    size_t count = 0;
    for (size_t i = 0; i < s_state->channel_count; i++) {
        ps_channel_state_t *ch = &s_state->channels[i];
        if (strcmp(ch->channel_id, channel_id) == 0) {
            count = (ch->cache ? ch->cache->entry_count : ch->entry_count);
            break;
        }
    }

    xSemaphoreGive(s_state->mutex);
    return count;
}

esp_err_t play_scheduler_get_channel_entry(const char *channel_id, size_t index,
                                            makapix_channel_entry_t *out_entry)
{
    ps_state_t *s_state = ps_get_state();

    if (!channel_id || !out_entry || !s_state->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);

    esp_err_t result = ESP_ERR_NOT_FOUND;

    for (size_t i = 0; i < s_state->channel_count; i++) {
        ps_channel_state_t *ch = &s_state->channels[i];
        if (strcmp(ch->channel_id, channel_id) == 0) {
            // Only Makapix and Giphy channels have makapix_channel_entry_t-compatible format
            if (ch->entry_format != PS_ENTRY_FORMAT_MAKAPIX && ch->entry_format != PS_ENTRY_FORMAT_GIPHY) {
                result = ESP_ERR_NOT_SUPPORTED;
                break;
            }

            // For Makapix channels, access cache directly to avoid stale pointers
            if (!ch->cache) {
                result = ESP_ERR_NOT_FOUND;
                break;
            }

            // Found channel - check index bounds using current cache values
            if (index >= ch->cache->entry_count || !ch->cache->entries) {
                result = ESP_ERR_NOT_FOUND;
                break;
            }

            // Copy entry to caller's buffer
            memcpy(out_entry, &ch->cache->entries[index], sizeof(makapix_channel_entry_t));
            result = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_state->mutex);
    return result;
}

bool play_scheduler_is_makapix_channel(const char *channel_id)
{
    ps_state_t *s_state = ps_get_state();

    if (!channel_id || !s_state->initialized) {
        return false;
    }

    // Look up channel type from active state
    for (size_t i = 0; i < s_state->channel_count; i++) {
        if (strcmp(s_state->channels[i].channel_id, channel_id) == 0) {
            ps_channel_type_t t = s_state->channels[i].type;
            return (t != PS_CHANNEL_TYPE_SDCARD &&
                    t != PS_CHANNEL_TYPE_GIPHY &&
                    t != PS_CHANNEL_TYPE_ARTWORK);
        }
    }
    return false;
}

bool play_scheduler_needs_download(const char *channel_id)
{
    ps_state_t *s_state = ps_get_state();

    if (!channel_id || !s_state->initialized) return false;

    // Look up channel type — SD card has local files, no download needed
    for (size_t i = 0; i < s_state->channel_count; i++) {
        if (strcmp(s_state->channels[i].channel_id, channel_id) == 0) {
            return (s_state->channels[i].type != PS_CHANNEL_TYPE_SDCARD);
        }
    }
    return false;
}

bool play_scheduler_is_giphy_channel(const char *channel_id)
{
    ps_state_t *s_state = ps_get_state();

    if (!channel_id || !s_state->initialized) return false;

    for (size_t i = 0; i < s_state->channel_count; i++) {
        if (strcmp(s_state->channels[i].channel_id, channel_id) == 0) {
            return (s_state->channels[i].type == PS_CHANNEL_TYPE_GIPHY);
        }
    }
    return false;
}
