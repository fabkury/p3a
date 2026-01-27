// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_commands.c
 * @brief Play Scheduler - Playset execution and cache loading
 *
 * This file implements playset (scheduler command) execution including:
 * - Channel cache loading (SD card and Makapix formats)
 * - execute_command() for multi-channel playset setup
 * - Convenience functions for named/user/hashtag channels
 *
 * A "playset" is the preferred term for a scheduler command - see
 * play_scheduler_types.h for the full definition.
 */

#include "play_scheduler.h"
#include "play_scheduler_internal.h"
#include "channel_cache.h"
#include "view_tracker.h"
#include "config_store.h"
#include "p3a_state.h"
#include "sd_path.h"
#include "content_cache.h"
#include "makapix.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *TAG = "ps_commands";

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Sanitize an identifier for filesystem safety
 *
 * Replaces non-alphanumeric characters with underscore.
 */
static void ps_sanitize_identifier(const char *input, char *output, size_t max_len)
{
    size_t i = 0;
    size_t j = 0;
    while (input[i] && j < max_len - 1) {
        char c = input[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            output[j++] = c;
        } else {
            output[j++] = '_';
        }
        i++;
    }
    output[j] = '\0';
}

/**
 * @brief Build channel_id from channel spec
 *
 * Format:
 * - NAMED: "{name}" -> "all", "promoted"
 * - USER: "by_user_{sqid}" -> "by_user_uvz"
 * - HASHTAG: "hashtag_{tag}" -> "hashtag_sunset"
 * - SDCARD: "sdcard"
 */
static void ps_build_channel_id(const ps_channel_spec_t *spec, char *out_id, size_t max_len)
{
    char sanitized[33];

    switch (spec->type) {
        case PS_CHANNEL_TYPE_NAMED:
            snprintf(out_id, max_len, "%s", spec->name);
            break;

        case PS_CHANNEL_TYPE_USER:
            ps_sanitize_identifier(spec->identifier, sanitized, sizeof(sanitized));
            snprintf(out_id, max_len, "by_user_%s", sanitized);
            break;

        case PS_CHANNEL_TYPE_HASHTAG:
            ps_sanitize_identifier(spec->identifier, sanitized, sizeof(sanitized));
            snprintf(out_id, max_len, "hashtag_%s", sanitized);
            break;

        case PS_CHANNEL_TYPE_SDCARD:
            snprintf(out_id, max_len, "sdcard");
            break;

        case PS_CHANNEL_TYPE_ARTWORK:
            snprintf(out_id, max_len, "artwork");
            break;

        default:
            snprintf(out_id, max_len, "unknown");
            break;
    }
}

// ============================================================================
// Cache Path Building
// ============================================================================

void ps_build_cache_path(const char *channel_id, char *out_path, size_t max_len)
{
    char channel_dir[256];
    if (sd_path_get_channel(channel_dir, sizeof(channel_dir)) != ESP_OK) {
        strlcpy(channel_dir, "/sdcard/p3a/channel", sizeof(channel_dir));
    }

    // For user/hashtag channels, replace : with _ in filename
    char safe_id[64];
    size_t j = 0;
    for (size_t i = 0; channel_id[i] && j < sizeof(safe_id) - 1; i++) {
        safe_id[j++] = (channel_id[i] == ':') ? '_' : channel_id[i];
    }
    safe_id[j] = '\0';

    // NOTE: Callers should use a sufficiently large output buffer (>= 512 bytes)
    // to avoid path truncation warnings treated as errors under -Wformat-truncation.
    snprintf(out_path, max_len, "%s/%s.bin", channel_dir, safe_id);
}

// ============================================================================
// Cache Loading
// ============================================================================

/**
 * @brief Load SD card channel cache (raw binary format)
 */
static esp_err_t ps_load_sdcard_cache(ps_channel_state_t *ch)
{
    char cache_path[512];
    ps_build_cache_path(ch->channel_id, cache_path, sizeof(cache_path));

    struct stat st;
    if (stat(cache_path, &st) != 0) {
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        ESP_LOGD(TAG, "Channel '%s': no cache file", ch->channel_id);
        return ESP_ERR_NOT_FOUND;
    }

    size_t entry_size = sizeof(sdcard_index_entry_t);  // 160 bytes

    if (st.st_size <= 0 || st.st_size % entry_size != 0) {
        ESP_LOGW(TAG, "Channel '%s': invalid cache file size %ld (expected multiple of %zu)",
                 ch->channel_id, (long)st.st_size, entry_size);
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        return ESP_ERR_INVALID_SIZE;
    }

    ch->entry_count = st.st_size / entry_size;

    if (ch->entries) {
        free(ch->entries);
        ch->entries = NULL;
    }

    ch->entries = malloc(ch->entry_count * entry_size);
    if (!ch->entries) {
        ESP_LOGE(TAG, "Channel '%s': failed to allocate %zu entries", ch->channel_id, ch->entry_count);
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(cache_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Channel '%s': failed to open cache file", ch->channel_id);
        free(ch->entries);
        ch->entries = NULL;
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        return ESP_FAIL;
    }

    size_t read_count = fread(ch->entries, entry_size, ch->entry_count, f);
    fclose(f);

    if (read_count != ch->entry_count) {
        ESP_LOGE(TAG, "Channel '%s': read %zu/%zu entries", ch->channel_id, read_count, ch->entry_count);
        free(ch->entries);
        ch->entries = NULL;
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        return ESP_FAIL;
    }

    ch->cache_loaded = true;
    ch->active = (ch->entry_count > 0);
    ch->entry_format = PS_ENTRY_FORMAT_SDCARD;

    ps_touch_cache_file(ch->channel_id);

    ESP_LOGI(TAG, "Channel '%s': loaded cache with %zu entries (sdcard format) into memory",
             ch->channel_id, ch->entry_count);

    if (ch->entry_count > 0) {
        sdcard_index_entry_t *entries = (sdcard_index_entry_t *)ch->entries;
        ESP_LOGI(TAG, "First SD card entry: post_id=%ld, ext=%d, filename='%s'",
                 (long)entries[0].post_id, entries[0].extension, entries[0].filename);
    }

    return ESP_OK;
}

/**
 * @brief Load Makapix channel cache using channel_cache module
 *
 * Uses the unified cache file format with LAi persistence. On first load after
 * firmware upgrade (legacy format), LAi is rebuilt once and saved in new format.
 */
static esp_err_t ps_load_makapix_cache(ps_channel_state_t *ch)
{
    // Get paths
    char channels_path[128];
    char vault_path[128];
    if (sd_path_get_channel(channels_path, sizeof(channels_path)) != ESP_OK) {
        strlcpy(channels_path, "/sdcard/p3a/channel", sizeof(channels_path));
    }
    if (sd_path_get_vault(vault_path, sizeof(vault_path)) != ESP_OK) {
        strlcpy(vault_path, "/sdcard/p3a/vault", sizeof(vault_path));
    }

    // Free existing cache if any (handles channel switch)
    if (ch->cache) {
        channel_cache_unregister(ch->cache);
        channel_cache_free(ch->cache);
        free(ch->cache);
        ch->cache = NULL;
        ch->entries = NULL;
        ch->available_post_ids = NULL;
    }

    // Allocate new cache structure
    ch->cache = malloc(sizeof(channel_cache_t));
    if (!ch->cache) {
        ESP_LOGE(TAG, "Channel '%s': failed to allocate cache structure", ch->channel_id);
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->available_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        return ESP_ERR_NO_MEM;
    }

    // Load cache (handles legacy format migration, CRC validation, LAi persistence)
    esp_err_t err = channel_cache_load(ch->channel_id, channels_path, vault_path, ch->cache);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Channel '%s': channel_cache_load failed: %s",
                 ch->channel_id, esp_err_to_name(err));
        free(ch->cache);
        ch->cache = NULL;
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->available_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        return err;
    }

    // Register for debounced persistence
    channel_cache_register(ch->cache);

    // If cache was migrated from legacy format (dirty flag set), schedule save in new format
    if (ch->cache->dirty) {
        ESP_LOGI(TAG, "Channel '%s': migrated from legacy format, scheduling save", ch->channel_id);
        channel_cache_schedule_save(ch->cache);
    }

    // For Makapix channels, all code accesses ch->cache->* directly to avoid stale
    // pointers when cache arrays are reallocated during batch merges.
    ch->entries = NULL;
    ch->entry_count = 0;
    ch->available_post_ids = NULL;
    ch->available_count = 0;

    ch->cache_loaded = true;
    ch->active = (ch->cache->available_count > 0);
    ch->entry_format = PS_ENTRY_FORMAT_MAKAPIX;

    ps_touch_cache_file(ch->channel_id);

    ESP_LOGI(TAG, "Channel '%s': loaded cache with %zu entries, %zu available (makapix format)",
             ch->channel_id, ch->cache->entry_count, ch->cache->available_count);

    return ESP_OK;
}

/**
 * @brief Load cache file for a channel
 *
 * Loads .bin file if it exists and sets entry_count and active flag.
 * Channels without cache get weight=0 until refresh completes.
 *
 * SD card channels use sdcard_index_entry_t (160 bytes per entry).
 * Makapix channels use makapix_channel_entry_t (64 bytes per entry).
 */
esp_err_t ps_load_channel_cache(ps_channel_state_t *ch)
{
    // Artwork channels are in-memory only - no cache file
    if (ch->type == PS_CHANNEL_TYPE_ARTWORK) {
        ch->cache_loaded = true;  // Mark as "loaded" (single entry from spec)
        ch->entry_count = 1;
        ch->active = false;  // Will become active after download check/completion
        ch->entry_format = PS_ENTRY_FORMAT_NONE;  // No standard entry format
        return ESP_OK;
    }

    // SD card channels use raw binary format (no LAi needed - files are always local)
    if (ch->type == PS_CHANNEL_TYPE_SDCARD) {
        return ps_load_sdcard_cache(ch);
    }

    // Makapix channels use channel_cache module for LAi persistence
    return ps_load_makapix_cache(ch);
}

// ============================================================================
// Command Execution
// ============================================================================

esp_err_t play_scheduler_execute_command(const ps_scheduler_command_t *command)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!command || command->channel_count == 0 || command->channel_count > PS_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    // Cancel all active Makapix refresh tasks before setting up new channels
    // This prevents old refresh tasks from wasting MQTT queries when switching channels
    makapix_cancel_all_refreshes();

    // Reset the periodic refresh timer so this command triggers immediate refresh
    ps_refresh_reset_timer();

    // Stop view tracking for the old channel before switching
    // This prevents view events from being sent for the wrong channel
    view_tracker_stop();

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "Executing scheduler command: %zu channel(s), exposure=%d, pick=%d",
             command->channel_count, command->exposure_mode, command->pick_mode);

    // Free old channel entries before reconfiguring
    for (size_t i = 0; i < s_state->channel_count; i++) {
        ps_channel_state_t *ch = &s_state->channels[i];
        if (ch->cache) {
            // Makapix channel - cache owns all memory
            channel_cache_unregister(ch->cache);
            channel_cache_free(ch->cache);
            free(ch->cache);
            ch->cache = NULL;
            ch->entries = NULL;
            ch->available_post_ids = NULL;
            ch->available_count = 0;
        } else if (ch->entries) {
            // SD card channel - entries owned directly
            free(ch->entries);
            ch->entries = NULL;
        }
    }

    // Store command parameters
    s_state->exposure_mode = command->exposure_mode;
    s_state->pick_mode = command->pick_mode;
    s_state->channel_count = command->channel_count;

    // Increment epoch (history is preserved)
    s_state->epoch_id++;

    // Initialize each channel
    for (size_t i = 0; i < command->channel_count; i++) {
        const ps_channel_spec_t *spec = &command->channels[i];
        ps_channel_state_t *ch = &s_state->channels[i];

        // Build channel_id from spec
        ps_build_channel_id(spec, ch->channel_id, sizeof(ch->channel_id));
        ch->type = spec->type;

        // Reset SWRR state
        ch->credit = 0;
        ch->weight = spec->weight;  // Will be recalculated after cache load

        // Reset pick state
        ch->cursor = 0;
        ps_prng_seed(&ch->pick_rng_state, s_state->global_seed ^ (uint32_t)i ^ s_state->epoch_id);

        // Clear legacy handle
        ch->handle = NULL;

        // Reset refresh state
        ch->refresh_pending = true;  // Queue for background refresh
        ch->refresh_in_progress = false;
        ch->total_count = 0;
        ch->recent_count = 0;

        // Initialize artwork-specific state if this is an artwork channel
        if (spec->type == PS_CHANNEL_TYPE_ARTWORK) {
            ch->artwork_state.post_id = spec->artwork.post_id;
            strlcpy(ch->artwork_state.storage_key, spec->artwork.storage_key, sizeof(ch->artwork_state.storage_key));
            strlcpy(ch->artwork_state.art_url, spec->artwork.art_url, sizeof(ch->artwork_state.art_url));
            strlcpy(ch->artwork_state.filepath, spec->artwork.filepath, sizeof(ch->artwork_state.filepath));
            ch->artwork_state.download_pending = true;  // Will check/download in refresh
            ch->artwork_state.download_in_progress = false;
            ch->refresh_pending = true;  // Trigger refresh to check/download
        }

        // Load existing cache if available
        ps_load_channel_cache(ch);

        size_t entries_count = (ch->cache ? ch->cache->entry_count : ch->entry_count);
        ESP_LOGD(TAG, "Channel[%zu]: id='%s', type=%d, weight=%lu, active=%d, entries=%zu",
                 i, ch->channel_id, ch->type, (unsigned long)ch->weight,
                 ch->active, entries_count);
    }

    // Calculate SWRR weights
    ps_swrr_calculate_weights(s_state);

    // Store first channel as "current" for status display
    if (command->channel_count > 0) {
        strlcpy(s_state->current_channel_id, s_state->channels[0].channel_id,
                sizeof(s_state->current_channel_id));

        // Update p3a_state with the new channel for view tracker and status API
        const ps_channel_spec_t *spec = &command->channels[0];
        if (spec->type == PS_CHANNEL_TYPE_SDCARD) {
            p3a_state_switch_channel(P3A_CHANNEL_SDCARD, NULL);
        } else if (spec->type == PS_CHANNEL_TYPE_NAMED) {
            if (strcmp(spec->name, "all") == 0) {
                p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_ALL, NULL);
            } else if (strcmp(spec->name, "promoted") == 0) {
                p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_PROMOTED, NULL);
            }
        } else if (spec->type == PS_CHANNEL_TYPE_USER) {
            p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_BY_USER, spec->identifier);
        } else if (spec->type == PS_CHANNEL_TYPE_HASHTAG) {
            p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_HASHTAG, spec->identifier);
        } else if (spec->type == PS_CHANNEL_TYPE_ARTWORK) {
            p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_ARTWORK, NULL);
        }
    }

    // Signal background refresh task to process pending channels
    ps_refresh_signal_work();

    // Update content cache with new channel list for round-robin downloading
    const char *channel_ids[PS_MAX_CHANNELS];
    for (size_t i = 0; i < command->channel_count; i++) {
        channel_ids[i] = s_state->channels[i].channel_id;
    }
    content_cache_set_channels(channel_ids, command->channel_count);

    // Reset playback_initiated so cache can trigger playback for new channel
    content_cache_reset_playback_initiated();

    // Check if any channel has entries we can play immediately
    bool has_entries = false;
    char first_channel_display_name[64] = "Channel";
    for (size_t i = 0; i < s_state->channel_count; i++) {
        ps_channel_state_t *ch = &s_state->channels[i];
        size_t entry_count = (ch->cache ? ch->cache->entry_count : ch->entry_count);
        if (ch->active && entry_count > 0) {
            has_entries = true;
            break;
        }
    }
    // Get first channel's display name for UI
    if (s_state->channel_count > 0) {
        ps_get_display_name(s_state->channels[0].channel_id, first_channel_display_name, sizeof(first_channel_display_name));
    }

    xSemaphoreGive(s_state->mutex);

    // Only trigger initial playback if we have entries
    // Otherwise, let download manager trigger it when first file is available
    if (has_entries) {
        return play_scheduler_next(NULL);
    } else {
        ESP_LOGI(TAG, "No cached entries yet - waiting for refresh/download");

        // Show loading state to user while waiting for refresh/download
        // But only if we have WiFi connectivity (no point showing loading in AP mode)
        if (p3a_state_has_wifi()) {
            extern void p3a_render_set_channel_message(const char *channel_name, int msg_type,
                                                       int progress_percent, const char *detail);
            p3a_render_set_channel_message(first_channel_display_name, 1 /* P3A_CHANNEL_MSG_LOADING */, -1,
                                            "Loading channel...");
        }
        return ESP_OK;
    }
}

// ============================================================================
// Convenience Functions
// ============================================================================

esp_err_t play_scheduler_play_named_channel(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_named_channel: %s", name);

    // Heap allocate to avoid ~4.6KB stack usage (called from 8KB stack tasks)
    ps_scheduler_command_t *cmd = calloc(1, sizeof(ps_scheduler_command_t));
    if (!cmd) {
        ESP_LOGE(TAG, "Failed to allocate command struct");
        return ESP_ERR_NO_MEM;
    }

    cmd->channel_count = 1;
    cmd->exposure_mode = PS_EXPOSURE_EQUAL;
    cmd->pick_mode = (config_store_get_play_order() == 2) ? PS_PICK_RANDOM : PS_PICK_RECENCY;

    // Determine channel type
    if (strcmp(name, "sdcard") == 0) {
        cmd->channels[0].type = PS_CHANNEL_TYPE_SDCARD;
        strlcpy(cmd->channels[0].name, "sdcard", sizeof(cmd->channels[0].name));
    } else {
        cmd->channels[0].type = PS_CHANNEL_TYPE_NAMED;
        strlcpy(cmd->channels[0].name, name, sizeof(cmd->channels[0].name));
    }
    cmd->channels[0].weight = 1;

    esp_err_t result = play_scheduler_execute_command(cmd);
    free(cmd);
    return result;
}

esp_err_t play_scheduler_play_user_channel(const char *user_sqid)
{
    if (!user_sqid) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_user_channel: %s", user_sqid);

    // Heap allocate to avoid ~4.6KB stack usage (called from 8KB stack tasks)
    ps_scheduler_command_t *cmd = calloc(1, sizeof(ps_scheduler_command_t));
    if (!cmd) {
        ESP_LOGE(TAG, "Failed to allocate command struct");
        return ESP_ERR_NO_MEM;
    }

    cmd->channel_count = 1;
    cmd->exposure_mode = PS_EXPOSURE_EQUAL;
    cmd->pick_mode = (config_store_get_play_order() == 2) ? PS_PICK_RANDOM : PS_PICK_RECENCY;

    cmd->channels[0].type = PS_CHANNEL_TYPE_USER;
    strlcpy(cmd->channels[0].name, "user", sizeof(cmd->channels[0].name));
    strlcpy(cmd->channels[0].identifier, user_sqid, sizeof(cmd->channels[0].identifier));
    cmd->channels[0].weight = 1;

    esp_err_t result = play_scheduler_execute_command(cmd);
    free(cmd);
    return result;
}

esp_err_t play_scheduler_play_hashtag_channel(const char *hashtag)
{
    if (!hashtag) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_hashtag_channel: %s", hashtag);

    // Heap allocate to avoid ~4.6KB stack usage (called from 8KB stack tasks)
    ps_scheduler_command_t *cmd = calloc(1, sizeof(ps_scheduler_command_t));
    if (!cmd) {
        ESP_LOGE(TAG, "Failed to allocate command struct");
        return ESP_ERR_NO_MEM;
    }

    cmd->channel_count = 1;
    cmd->exposure_mode = PS_EXPOSURE_EQUAL;
    cmd->pick_mode = (config_store_get_play_order() == 2) ? PS_PICK_RANDOM : PS_PICK_RECENCY;

    cmd->channels[0].type = PS_CHANNEL_TYPE_HASHTAG;
    strlcpy(cmd->channels[0].name, "hashtag", sizeof(cmd->channels[0].name));
    strlcpy(cmd->channels[0].identifier, hashtag, sizeof(cmd->channels[0].identifier));
    cmd->channels[0].weight = 1;

    esp_err_t result = play_scheduler_execute_command(cmd);
    free(cmd);
    return result;
}

esp_err_t play_scheduler_refresh_sdcard_cache(void)
{
    ESP_LOGI(TAG, "Refreshing SD card cache");
    return ps_build_sdcard_index();
}

// ============================================================================
// Artwork Channel Functions
// ============================================================================

/**
 * @brief Build vault filepath from storage_key and art_url
 *
 * Computes the sharded vault path: {vault}/{sha[0]}/{sha[1]}/{sha[2]}/{storage_key}.{ext}
 */
static void ps_build_artwork_filepath(const char *storage_key, const char *art_url,
                                       char *out_path, size_t max_len)
{
    if (!storage_key || !out_path || max_len == 0) {
        if (out_path && max_len > 0) out_path[0] = '\0';
        return;
    }

    char vault_base[128];
    if (sd_path_get_vault(vault_base, sizeof(vault_base)) != ESP_OK) {
        strlcpy(vault_base, "/sdcard/p3a/vault", sizeof(vault_base));
    }

    // Compute SHA256 for sharding
    uint8_t sha256[32];
    if (mbedtls_sha256((const unsigned char *)storage_key, strlen(storage_key), sha256, 0) != 0) {
        // Fallback without sharding
        snprintf(out_path, max_len, "%s/%s.webp", vault_base, storage_key);
        return;
    }

    // Detect extension from URL
    const char *ext = ".webp";
    if (art_url) {
        size_t url_len = strlen(art_url);
        if (url_len >= 4) {
            if (strcasecmp(art_url + url_len - 4, ".gif") == 0) ext = ".gif";
            else if (strcasecmp(art_url + url_len - 4, ".png") == 0) ext = ".png";
            else if (strcasecmp(art_url + url_len - 4, ".jpg") == 0) ext = ".jpg";
            else if (url_len >= 5 && strcasecmp(art_url + url_len - 5, ".jpeg") == 0) ext = ".jpg";
            else if (url_len >= 5 && strcasecmp(art_url + url_len - 5, ".webp") == 0) ext = ".webp";
        }
    }

    snprintf(out_path, max_len, "%s/%02x/%02x/%02x/%s%s",
             vault_base,
             (unsigned int)sha256[0],
             (unsigned int)sha256[1],
             (unsigned int)sha256[2],
             storage_key, ext);
}

esp_err_t play_scheduler_play_artwork(int32_t post_id, const char *storage_key, const char *art_url)
{
    if (!storage_key || !art_url) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_artwork: post_id=%ld, storage_key=%s", (long)post_id, storage_key);

    // Set view intent BEFORE execute_command (for view tracking)
    extern void makapix_set_view_intent_intentional(bool intentional);
    makapix_set_view_intent_intentional(true);

    // Heap allocate to avoid ~4.6KB stack usage
    ps_scheduler_command_t *cmd = calloc(1, sizeof(ps_scheduler_command_t));
    if (!cmd) {
        ESP_LOGE(TAG, "Failed to allocate command struct");
        return ESP_ERR_NO_MEM;
    }

    cmd->channel_count = 1;
    cmd->exposure_mode = PS_EXPOSURE_EQUAL;
    cmd->pick_mode = PS_PICK_RECENCY;

    cmd->channels[0].type = PS_CHANNEL_TYPE_ARTWORK;
    strlcpy(cmd->channels[0].name, "artwork", sizeof(cmd->channels[0].name));
    cmd->channels[0].weight = 1;

    // Set artwork-specific fields
    cmd->channels[0].artwork.post_id = post_id;
    strlcpy(cmd->channels[0].artwork.storage_key, storage_key, sizeof(cmd->channels[0].artwork.storage_key));
    strlcpy(cmd->channels[0].artwork.art_url, art_url, sizeof(cmd->channels[0].artwork.art_url));

    // Compute vault filepath from storage_key
    ps_build_artwork_filepath(storage_key, art_url,
                               cmd->channels[0].artwork.filepath,
                               sizeof(cmd->channels[0].artwork.filepath));

    esp_err_t result = play_scheduler_execute_command(cmd);
    free(cmd);
    return result;
}

esp_err_t play_scheduler_play_local_file(const char *filepath)
{
    if (!filepath) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_local_file: %s", filepath);

    // Heap allocate to avoid ~4.6KB stack usage
    ps_scheduler_command_t *cmd = calloc(1, sizeof(ps_scheduler_command_t));
    if (!cmd) {
        ESP_LOGE(TAG, "Failed to allocate command struct");
        return ESP_ERR_NO_MEM;
    }

    cmd->channel_count = 1;
    cmd->exposure_mode = PS_EXPOSURE_EQUAL;
    cmd->pick_mode = PS_PICK_RECENCY;

    cmd->channels[0].type = PS_CHANNEL_TYPE_ARTWORK;
    strlcpy(cmd->channels[0].name, "artwork", sizeof(cmd->channels[0].name));
    cmd->channels[0].weight = 1;

    // Local files: no view tracking (post_id = 0), storage_key and art_url empty
    cmd->channels[0].artwork.post_id = 0;
    cmd->channels[0].artwork.storage_key[0] = '\0';
    cmd->channels[0].artwork.art_url[0] = '\0';
    strlcpy(cmd->channels[0].artwork.filepath, filepath, sizeof(cmd->channels[0].artwork.filepath));

    esp_err_t result = play_scheduler_execute_command(cmd);
    free(cmd);
    return result;
}
