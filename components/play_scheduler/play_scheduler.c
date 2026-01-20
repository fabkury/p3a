// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler.c
 * @brief Play Scheduler - Core implementation
 *
 * This file implements the core Play Scheduler logic including:
 * - Initialization and deinitialization
 * - Global state management
 * - Channel configuration and loading
 * - Legacy API (set_channels, play_channel)
 * - Stats and debugging
 *
 * Other functionality is split into separate files:
 * - play_scheduler_commands.c: Command execution
 * - play_scheduler_navigation.c: Navigation and swap requests
 * - play_scheduler_lai.c: LAi operations and download integration
 *
 * @see docs/play-scheduler/SPECIFICATION.md
 */

#include "play_scheduler.h"
#include "play_scheduler_internal.h"
#include "channel_interface.h"
#include "channel_cache.h"
#include "sdcard_channel_impl.h"
#include "makapix_channel_impl.h"
#include "config_store.h"
#include "p3a_state.h"
#include "sd_path.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "play_scheduler";

// ============================================================================
// DEFERRED: Live Mode Synchronized Playback
// ============================================================================
//
// Live Mode was a feature for synchronized playback across multiple devices.
// Key concepts that were in the deprecated play_navigator.c:
//
// - live_mode flag on navigator: Indicates synchronized playback is active
// - live_p/live_q arrays: Flattened schedule of (post, artwork) indices
// - live_count: Number of items in the flattened schedule
// - live_ready: Whether the schedule has been built and is valid
//
// Key functions that existed:
// - play_navigator_set_live_mode(): Enable/disable synchronized playback
// - play_navigator_mark_live_dirty(): Signal schedule needs rebuild
// - Schedule calculation based on SNTP-synchronized wall clock time
//
// When implementing Live Mode in Play Scheduler:
// 1. Add live_mode flag to ps_state_t
// 2. Use SNTP time sync for coordination (sntp_sync.h)
// 3. Build flattened schedule from lookahead entries
// 4. Calculate start_time_ms and start_frame for swap requests
// 5. Wire into swap_future.c for scheduled swaps
//
// See docs/LIVE_MODE_ANALYSIS.md for full analysis.
// ============================================================================

// ============================================================================
// Global State
// ============================================================================

static ps_state_t s_state = {0};

ps_state_t *ps_get_state(void)
{
    return &s_state;
}

// ============================================================================
// Shared Utility Functions
// ============================================================================

bool ps_file_exists(const char *path)
{
    if (!path || path[0] == '\0') return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

void ps_get_display_name(const char *channel_id, char *out_name, size_t max_len)
{
    if (!channel_id || !out_name || max_len == 0) return;
    
    if (strcmp(channel_id, "all") == 0) {
        snprintf(out_name, max_len, "All Artworks");
    } else if (strcmp(channel_id, "promoted") == 0) {
        snprintf(out_name, max_len, "Promoted");
    } else if (strcmp(channel_id, "user") == 0) {
        snprintf(out_name, max_len, "My Channel");
    } else if (strcmp(channel_id, "sdcard") == 0) {
        snprintf(out_name, max_len, "microSD Card");
    } else if (strncmp(channel_id, "by_user_", 8) == 0) {
        // Truncate user ID to fit in output buffer with "User: " prefix
        char truncated[48];
        strncpy(truncated, channel_id + 8, sizeof(truncated) - 1);
        truncated[sizeof(truncated) - 1] = '\0';
        snprintf(out_name, max_len, "User: %.48s", truncated);
    } else if (strncmp(channel_id, "hashtag_", 8) == 0) {
        // Truncate hashtag to fit in output buffer with "#" prefix
        char truncated[56];
        strncpy(truncated, channel_id + 8, sizeof(truncated) - 1);
        truncated[sizeof(truncated) - 1] = '\0';
        snprintf(out_name, max_len, "#%.56s", truncated);
    } else {
        // Truncate channel_id to fit in output buffer
        snprintf(out_name, max_len, "%.63s", channel_id);
    }
}

esp_err_t ps_storage_key_sha256(const char *storage_key, uint8_t out_sha256[32])
{
    if (!storage_key || !out_sha256) {
        return ESP_ERR_INVALID_ARG;
    }
    // SHA256(storage_key) used for vault sharding
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int rc = mbedtls_sha256_starts(&ctx, 0);
    if (rc != 0) {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }
    rc = mbedtls_sha256_update(&ctx, (const unsigned char *)storage_key, strlen(storage_key));
    if (rc != 0) {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }
    rc = mbedtls_sha256_finish(&ctx, out_sha256);
    mbedtls_sha256_free(&ctx);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

// ============================================================================
// Channel Loading (Legacy)
// ============================================================================

static channel_handle_t s_sdcard_channel = NULL;

static esp_err_t load_channel_by_id(const char *channel_id, channel_handle_t *out_handle)
{
    if (!channel_id || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;

    if (strcmp(channel_id, "sdcard") == 0) {
        // SD Card channel
        if (!s_sdcard_channel) {
            s_sdcard_channel = sdcard_channel_create("SD Card", NULL);
            if (!s_sdcard_channel) {
                ESP_LOGE(TAG, "Failed to create SD card channel");
                return ESP_ERR_NO_MEM;
            }
        }
        *out_handle = s_sdcard_channel;
        return ESP_OK;
    }

    // Makapix channels
    if (strcmp(channel_id, "all") == 0 ||
        strcmp(channel_id, "promoted") == 0) {
        char vault_path[256] = {0};
        char channel_path[256] = {0};
        if (sd_path_get_vault(vault_path, sizeof(vault_path)) != ESP_OK ||
            sd_path_get_channel(channel_path, sizeof(channel_path)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get SD paths for Makapix channel '%s'", channel_id);
            return ESP_FAIL;
        }

        const char *channel_name = (strcmp(channel_id, "all") == 0) ? "Recent Artworks" : "Promoted";

        channel_handle_t ch = makapix_channel_create(channel_id, channel_name, vault_path, channel_path);
        if (!ch) {
            ESP_LOGE(TAG, "Failed to create Makapix channel '%s'", channel_id);
            return ESP_ERR_NO_MEM;
        }

        *out_handle = ch;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Unknown channel_id: %s", channel_id);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t activate_channel(size_t channel_index)
{
    ps_channel_state_t *ch = &s_state.channels[channel_index];

    if (!ch->handle) {
        // Load channel
        channel_handle_t handle = NULL;
        esp_err_t err = load_channel_by_id(ch->channel_id, &handle);
        if (err != ESP_OK) {
            ch->active = false;
            ch->entry_count = 0;
            return err;
        }
        ch->handle = handle;
    }

    channel_handle_t handle = (channel_handle_t)ch->handle;

    // Request refresh and load
    channel_request_refresh(handle);
    esp_err_t err = channel_load(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load channel '%s': %s", ch->channel_id, esp_err_to_name(err));
        ch->active = false;
        ch->entry_count = 0;
        return err;
    }

    // Start playback
    channel_order_mode_t order = CHANNEL_ORDER_CREATED;  // Default to newest first
    uint8_t play_order = config_store_get_play_order();
    if (play_order == 2) {
        order = CHANNEL_ORDER_RANDOM;
    } else if (play_order == 0) {
        order = CHANNEL_ORDER_ORIGINAL;
    }

    err = channel_start_playback(handle, order, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start playback for '%s': %s", ch->channel_id, esp_err_to_name(err));
    }

    // Get stats
    channel_stats_t stats;
    if (channel_get_stats(handle, &stats) == ESP_OK) {
        ch->entry_count = stats.total_items;
        ch->active = (stats.total_items > 0);
    } else {
        ch->entry_count = 0;
        ch->active = false;
    }

    ESP_LOGI(TAG, "Channel '%s' activated with %zu entries", ch->channel_id, ch->entry_count);

    return ESP_OK;
}

// ============================================================================
// Initialization
// ============================================================================

esp_err_t play_scheduler_init(void)
{
    if (s_state.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Play Scheduler (H=%d)", PS_HISTORY_SIZE);

    // Create mutex
    s_state.mutex = xSemaphoreCreateMutex();
    if (!s_state.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Allocate history buffer
    s_state.history = calloc(PS_HISTORY_SIZE, sizeof(ps_artwork_t));
    if (!s_state.history) {
        ESP_LOGE(TAG, "Failed to allocate history buffer");
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Initialize history buffer
    ps_history_init(&s_state);

    // Initialize state
    s_state.nae_count = 0;
    s_state.nae_enabled = true;
    s_state.epoch_id = 0;
    s_state.last_played_id = 0;  // 0 won't match any valid post_id (Makapix=positive, SDcard=negative)
    s_state.exposure_mode = PS_EXPOSURE_EQUAL;
    s_state.pick_mode = PS_PICK_RECENCY;
    s_state.channel_count = 0;
    s_state.current_channel = NULL;
    s_state.command_active = false;

    // Load dwell time from NVS
    uint32_t dwell_ms = config_store_get_dwell_time();
    s_state.dwell_time_seconds = dwell_ms / 1000;

    // Initialize PRNG with random seed
    s_state.global_seed = esp_random();
    ps_prng_seed(&s_state.prng_nae_state, s_state.global_seed ^ 0x5A5A5A5A);
    ps_prng_seed(&s_state.prng_pick_state, s_state.global_seed ^ 0xA5A5A5A5);

    s_state.initialized = true;

    // Start auto-swap timer task
    esp_err_t timer_err = ps_timer_start(&s_state);
    if (timer_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start timer task: %s", esp_err_to_name(timer_err));
        // Continue anyway - auto-swap won't work but manual navigation will
    }

    // Start background refresh task
    esp_err_t refresh_err = ps_refresh_start();
    if (refresh_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start refresh task: %s", esp_err_to_name(refresh_err));
        // Continue anyway - refresh will happen on-demand
    }

    ESP_LOGI(TAG, "Play Scheduler initialized");

    return ESP_OK;
}

void play_scheduler_deinit(void)
{
    if (!s_state.initialized) {
        return;
    }

    ESP_LOGI(TAG, "Deinitializing Play Scheduler");

    // Stop background refresh task
    ps_refresh_stop();

    // Stop timer task if running
    ps_timer_stop(&s_state);

    if (s_state.mutex) {
        xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    }

    // Free channel entries
    // IMPORTANT: For Makapix channels, ch->entries is an ALIAS to ch->cache->entries
    // and must NOT be freed directly. The cache owns the memory.
    for (size_t i = 0; i < s_state.channel_count; i++) {
        ps_channel_state_t *ch = &s_state.channels[i];
        if (ch->cache) {
            // Makapix channel - cache owns entries/available_indices
            channel_cache_unregister(ch->cache);
            channel_cache_free(ch->cache);
            free(ch->cache);
            ch->cache = NULL;
            ch->entries = NULL;
            ch->available_indices = NULL;
            ch->available_count = 0;
        } else if (ch->entries) {
            // SD card channel - entries owned directly
            free(ch->entries);
            ch->entries = NULL;
        }
    }

    // Free history buffer
    if (s_state.history) {
        free(s_state.history);
        s_state.history = NULL;
    }

    s_state.initialized = false;

    if (s_state.mutex) {
        xSemaphoreGive(s_state.mutex);
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
    }

    ESP_LOGI(TAG, "Play Scheduler deinitialized");
}

bool play_scheduler_is_initialized(void)
{
    return s_state.initialized;
}

// ============================================================================
// Channel Configuration (Legacy API)
// ============================================================================

esp_err_t play_scheduler_set_channels(
    const ps_channel_config_t *channels,
    size_t count,
    ps_exposure_mode_t mode)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!channels || count == 0 || count > PS_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "Setting %zu channel(s), mode=%d", count, mode);

    // Free old channel entries before reconfiguring
    // IMPORTANT: For Makapix channels, ch->entries is an ALIAS to ch->cache->entries
    // and must NOT be freed directly. The cache owns the memory.
    for (size_t i = 0; i < s_state.channel_count; i++) {
        ps_channel_state_t *ch = &s_state.channels[i];
        if (ch->cache) {
            // Makapix channel - cache owns entries/available_indices
            channel_cache_unregister(ch->cache);
            channel_cache_free(ch->cache);
            free(ch->cache);
            ch->cache = NULL;
            ch->entries = NULL;
            ch->available_indices = NULL;
            ch->available_count = 0;
        } else if (ch->entries) {
            // SD card channel - entries owned directly
            free(ch->entries);
            ch->entries = NULL;
        }
    }

    s_state.exposure_mode = mode;
    s_state.channel_count = count;

    // Copy channel configurations
    for (size_t i = 0; i < count; i++) {
        strncpy(s_state.channels[i].channel_id, channels[i].channel_id,
                sizeof(s_state.channels[i].channel_id) - 1);
        s_state.channels[i].channel_id[sizeof(s_state.channels[i].channel_id) - 1] = '\0';
        s_state.channels[i].weight = channels[i].weight;
        s_state.channels[i].cursor = 0;
        s_state.channels[i].credit = 0;
        s_state.channels[i].active = false;
        s_state.channels[i].entry_count = 0;
        s_state.channels[i].handle = NULL;

        // Seed per-channel PRNG
        ps_prng_seed(&s_state.channels[i].pick_rng_state,
                     s_state.global_seed ^ (uint32_t)i ^ s_state.epoch_id);
    }

    // Reset on snapshot change (but preserve history)
    ps_nae_clear(&s_state);
    s_state.epoch_id++;

    // Reset per-channel state
    for (size_t i = 0; i < count; i++) {
        ps_pick_reset_channel(&s_state, i);
    }

    // Activate channels
    for (size_t i = 0; i < count; i++) {
        activate_channel(i);
    }

    // Store current channel for status
    if (count > 0) {
        strlcpy(s_state.current_channel_id, channels[0].channel_id,
                sizeof(s_state.current_channel_id));
        s_state.current_channel = (channel_handle_t)s_state.channels[0].handle;
    }

    xSemaphoreGive(s_state.mutex);

    return ESP_OK;
}

esp_err_t play_scheduler_play_channel(const char *channel_id)
{
    if (!channel_id) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_channel: %s", channel_id);

    ps_channel_config_t config = {0};
    strncpy(config.channel_id, channel_id, sizeof(config.channel_id) - 1);
    config.weight = 1;
    config.total_count = 0;
    config.recent_count = 0;

    esp_err_t err = play_scheduler_set_channels(&config, 1, PS_EXPOSURE_EQUAL);
    if (err != ESP_OK) {
        return err;
    }

    // Trigger initial generation and swap
    return play_scheduler_next(NULL);
}

void play_scheduler_set_pick_mode(ps_pick_mode_t mode)
{
    if (!s_state.initialized) return;
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.pick_mode = mode;
    xSemaphoreGive(s_state.mutex);
}

ps_pick_mode_t play_scheduler_get_pick_mode(void)
{
    return s_state.pick_mode;
}

// ============================================================================
// Status & Debugging
// ============================================================================

esp_err_t play_scheduler_get_stats(ps_stats_t *out_stats)
{
    if (!s_state.initialized || !out_stats) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    out_stats->channel_count = s_state.channel_count;
    out_stats->history_count = s_state.history_count;
    out_stats->lookahead_count = 0;  // No longer using lookahead buffer
    out_stats->nae_pool_count = s_state.nae_count;
    out_stats->epoch_id = s_state.epoch_id;
    out_stats->current_channel_id = s_state.channel_count > 0
        ? s_state.current_channel_id
        : NULL;

    xSemaphoreGive(s_state.mutex);

    return ESP_OK;
}

size_t play_scheduler_get_active_channel_ids(const char **out_ids, size_t max_count)
{
    if (!s_state.initialized || !out_ids || max_count == 0) {
        return 0;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    size_t count = s_state.channel_count;
    if (count > max_count) {
        count = max_count;
    }

    for (size_t i = 0; i < count; i++) {
        // Return pointer to internal storage (stable until next execute_command)
        out_ids[i] = s_state.channels[i].channel_id;
    }

    xSemaphoreGive(s_state.mutex);

    return count;
}

void play_scheduler_reset(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "Resetting scheduler (epoch %lu -> %lu)",
             (unsigned long)s_state.epoch_id,
             (unsigned long)(s_state.epoch_id + 1));

    // Clear NAE pool
    ps_nae_clear(&s_state);

    // Reset per-channel state (cursors, SWRR credits)
    for (size_t i = 0; i < s_state.channel_count; i++) {
        ps_pick_reset_channel(&s_state, i);
        s_state.channels[i].credit = 0;
    }

    // Increment epoch
    s_state.epoch_id++;

    // Note: History is preserved across resets

    xSemaphoreGive(s_state.mutex);
}
