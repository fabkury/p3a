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
#include "load_tracker.h"
#include "makapix_channel_utils.h"
#include "p3a_state.h"
#include "sd_path.h"
#include "content_cache.h"
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
 * @brief Find Ci index by storage_key (UUID string)
 */
static uint32_t ps_find_ci_by_storage_key(ps_channel_state_t *ch, const char *storage_key)
{
    if (!ch || !storage_key || !ch->entries || ch->entry_count == 0) {
        return UINT32_MAX;
    }

    // Only Makapix entries have storage_key
    if (ch->entry_format != PS_ENTRY_FORMAT_MAKAPIX) {
        return UINT32_MAX;
    }

    // Convert storage_key to UUID bytes
    uint8_t uuid_bytes[16];
    if (!uuid_to_bytes(storage_key, uuid_bytes)) {
        return UINT32_MAX;
    }

    makapix_channel_entry_t *entries = (makapix_channel_entry_t *)ch->entries;
    for (size_t i = 0; i < ch->entry_count; i++) {
        if (memcmp(entries[i].storage_key_uuid, uuid_bytes, 16) == 0) {
            return (uint32_t)i;
        }
    }

    return UINT32_MAX;
}

/**
 * @brief Check if a Ci index is already in LAi
 */
static bool ps_lai_contains(ps_channel_state_t *ch, uint32_t ci_index)
{
    if (!ch->available_indices) return false;

    for (size_t i = 0; i < ch->available_count; i++) {
        if (ch->available_indices[i] == ci_index) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Add a Ci index to LAi
 *
 * For Makapix channels, delegates to channel_cache module which handles
 * dirty tracking and debounced persistence.
 */
static bool ps_lai_add(ps_channel_state_t *ch, uint32_t ci_index)
{
    if (ci_index >= ch->entry_count) return false;

    // For Makapix channels with cache, use channel_cache module
    if (ch->cache) {
        bool added = lai_add_entry(ch->cache, ci_index);
        if (added) {
            // Update aliased pointer and count (pointer may have been allocated on first add)
            ch->available_indices = ch->cache->available_indices;
            ch->available_count = ch->cache->available_count;
            // Mark channel as active now that it has available content
            ch->active = true;
            // Schedule debounced save
            channel_cache_schedule_save(ch->cache);
        }
        return added;
    }

    // Fallback for SD card channels (shouldn't have LAi, but keep for safety)
    if (ps_lai_contains(ch, ci_index)) return false;

    if (!ch->available_indices) {
        ch->available_indices = malloc(ch->entry_count * sizeof(uint32_t));
        if (!ch->available_indices) return false;
        ch->available_count = 0;
    }

    ch->available_indices[ch->available_count++] = ci_index;
    ch->active = true;
    return true;
}

/**
 * @brief Remove a Ci index from LAi (swap-and-pop for O(1))
 *
 * For Makapix channels, delegates to channel_cache module which handles
 * dirty tracking and debounced persistence.
 */
static bool ps_lai_remove(ps_channel_state_t *ch, uint32_t ci_index)
{
    // For Makapix channels with cache, use channel_cache module
    if (ch->cache) {
        bool removed = lai_remove_entry(ch->cache, ci_index);
        if (removed) {
            // Update aliased count
            ch->available_count = ch->cache->available_count;
            // Schedule debounced save
            channel_cache_schedule_save(ch->cache);
        }
        return removed;
    }

    // Fallback for SD card channels
    if (!ch->available_indices || ch->available_count == 0) return false;

    for (size_t i = 0; i < ch->available_count; i++) {
        if (ch->available_indices[i] == ci_index) {
            ch->available_indices[i] = ch->available_indices[ch->available_count - 1];
            ch->available_count--;
            return true;
        }
    }
    return false;
}

// ============================================================================
// Download Completion Callback
// ============================================================================

void play_scheduler_on_download_complete(const char *channel_id, const char *storage_key)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized || !channel_id || !storage_key) {
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

    // Find the Ci index for this storage_key
    uint32_t ci_index = ps_find_ci_by_storage_key(ch, storage_key);
    if (ci_index == UINT32_MAX) {
        // Entry not found in current in-memory cache - the cache file may have been
        // updated by the refresh task. Reload the cache from disk and try again.
        ESP_LOGI(TAG, "Entry not in cache, reloading channel '%s' from disk", channel_id);
        esp_err_t reload_err = ps_load_channel_cache(ch);
        if (reload_err == ESP_OK) {
            // Recalculate SWRR weights after cache reload
            ps_swrr_calculate_weights(s_state);
            ci_index = ps_find_ci_by_storage_key(ch, storage_key);
        }
        
        if (ci_index == UINT32_MAX) {
            ESP_LOGD(TAG, "Downloaded file still not in Ci after reload: %s", storage_key);
            xSemaphoreGive(s_state->mutex);
            return;
        }
        
        // After reload, LAi is already rebuilt with currently-available files
        // So the downloaded file should already be in LAi
        ESP_LOGI(TAG, "Cache reloaded, entry found at ci=%lu, LAi has %zu entries",
                 (unsigned long)ci_index, ch->available_count);
        
        // Check for zero-to-one transition and trigger playback if needed
        size_t total_available = 0;
        for (size_t i = 0; i < s_state->channel_count; i++) {
            total_available += s_state->channels[i].available_count;
        }
        
        if (total_available > 0) {
            ESP_LOGI(TAG, "After cache reload - triggering playback (%zu total available)", total_available);
            xSemaphoreGive(s_state->mutex);
            event_bus_emit_simple(P3A_EVENT_SWAP_NEXT);
            return;
        }
        
        xSemaphoreGive(s_state->mutex);
        return;
    }

    // Track if this is a zero-to-one transition
    size_t prev_total_available = 0;
    for (size_t i = 0; i < s_state->channel_count; i++) {
        prev_total_available += s_state->channels[i].available_count;
    }

    // Add to LAi
    if (ps_lai_add(ch, ci_index)) {
        ESP_LOGI(TAG, "LAi add: ch='%s' ci=%lu, now %zu available",
                 channel_id, (unsigned long)ci_index, ch->available_count);

        // Check for zero-to-one transition
        if (prev_total_available == 0 && ch->available_count > 0) {
            ESP_LOGI(TAG, "Zero-to-one transition - triggering playback");
            xSemaphoreGive(s_state->mutex);

            // Trigger playback via event bus to avoid race condition
            event_bus_emit_simple(P3A_EVENT_SWAP_NEXT);
            return;
        }
    }

    xSemaphoreGive(s_state->mutex);
}

// ============================================================================
// Load Failure Handling
// ============================================================================

void play_scheduler_on_load_failed(const char *storage_key, const char *channel_id, const char *reason)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized || !storage_key) {
        return;
    }

    // Get vault path for LTF
    char vault_path[128];
    if (sd_path_get_vault(vault_path, sizeof(vault_path)) != ESP_OK) {
        strlcpy(vault_path, "/sdcard/p3a/vault", sizeof(vault_path));
    }

    // Record failure in LTF
    ltf_record_failure(storage_key, vault_path, reason ? reason : "unknown");

    // Build filepath and delete the corrupted file
    uint8_t sha256[32];
    if (ps_storage_key_sha256(storage_key, sha256) == ESP_OK) {
        // Build path: {vault}/{sha[0]:02x}/{sha[1]:02x}/{sha[2]:02x}/{storage_key}.{ext}
        // We need to try all extensions since we don't know which one it is
        const char *exts[] = {".webp", ".gif", ".png", ".jpg"};
        for (int i = 0; i < 4; i++) {
            char filepath[256];
            snprintf(filepath, sizeof(filepath), "%s/%02x/%02x/%02x/%s%s",
                     vault_path,
                     sha256[0], sha256[1], sha256[2],
                     storage_key, exts[i]);

            struct stat st;
            if (stat(filepath, &st) == 0) {
                unlink(filepath);
                ESP_LOGI(TAG, "Deleted corrupted file: %s", filepath);
                break;
            }
        }
    }

    // Remove from LAi if channel is known
    if (channel_id) {
        xSemaphoreTake(s_state->mutex, portMAX_DELAY);

        int ch_idx = ps_find_channel_index(s_state, channel_id);
        if (ch_idx >= 0) {
            ps_channel_state_t *ch = &s_state->channels[ch_idx];
            uint32_t ci_index = ps_find_ci_by_storage_key(ch, storage_key);
            if (ci_index != UINT32_MAX && ps_lai_remove(ch, ci_index)) {
                ESP_LOGI(TAG, "LAi remove: ch='%s' ci=%lu, now %zu available",
                         channel_id, (unsigned long)ci_index, ch->available_count);
            }
        }

        // Check if we need to try another artwork
        size_t total_available = 0;
        for (size_t i = 0; i < s_state->channel_count; i++) {
            total_available += s_state->channels[i].available_count;
        }

        xSemaphoreGive(s_state->mutex);

        // Try to pick another artwork if any available
        if (total_available > 0) {
            ESP_LOGI(TAG, "Trying another artwork after load failure");
            play_scheduler_next(NULL);
        } else {
            ESP_LOGW(TAG, "No artworks available after load failure");
            // Show appropriate message (only if we have WiFi)
            if (p3a_state_has_wifi()) {
                extern void p3a_render_set_channel_message(const char *channel_name, int msg_type,
                                                           int progress_percent, const char *detail);
                bool cache_busy = content_cache_is_busy();
                
                // Get display name for first channel
                char ch_display_name[64] = "Channel";
                xSemaphoreTake(s_state->mutex, portMAX_DELAY);
                if (s_state->channel_count > 0) {
                    ps_get_display_name(s_state->channels[0].channel_id, ch_display_name, sizeof(ch_display_name));
                }
                xSemaphoreGive(s_state->mutex);
                
                if (cache_busy) {
                    p3a_render_set_channel_message(ch_display_name, 2 /* P3A_CHANNEL_MSG_DOWNLOADING */, -1,
                                                    "Downloading artwork...");
                } else {
                    p3a_render_set_channel_message(NULL, 0, -1, NULL);
                }
            }
        }
    }
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
        total += s_state->channels[i].available_count;
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
        if (strcmp(s_state->channels[i].channel_id, channel_id) == 0) {
            if (out_total) *out_total = s_state->channels[i].entry_count;
            if (out_cached) *out_cached = s_state->channels[i].available_count;
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
        if (strcmp(s_state->channels[i].channel_id, channel_id) == 0) {
            count = s_state->channels[i].entry_count;
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
            // Found channel - check index bounds
            if (index >= ch->entry_count || !ch->entries) {
                result = ESP_ERR_NOT_FOUND;
                break;
            }

            // Only Makapix channels have makapix_channel_entry_t format
            if (ch->entry_format != PS_ENTRY_FORMAT_MAKAPIX) {
                result = ESP_ERR_NOT_SUPPORTED;
                break;
            }

            // Copy entry to caller's buffer
            // Must cast from void* to get correct pointer arithmetic
            makapix_channel_entry_t *entries = (makapix_channel_entry_t *)ch->entries;
            memcpy(out_entry, &entries[index], sizeof(makapix_channel_entry_t));
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

    // SD card channel is the only non-Makapix channel
    if (strcmp(channel_id, "sdcard") == 0) {
        return false;
    }

    // All other channels (all, promoted, user, by_user_*, hashtag_*) are Makapix channels
    return true;
}
