// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler.c
 * @brief Play Scheduler - Core implementation
 *
 * This file implements the main Play Scheduler logic including:
 * - Initialization and deinitialization
 * - Channel configuration and loading
 * - Navigation (next/prev/current) with availability masking
 * - Integration with animation_player
 *
 * Availability Masking: The scheduler only sees files that exist locally.
 * Entries without files are invisible - computed fresh on each pick.
 *
 * @see docs/play-scheduler/SPECIFICATION.md
 */

#include "play_scheduler.h"
#include "play_scheduler_internal.h"
#include "channel_interface.h"
#include "channel_cache.h"
#include "load_tracker.h"
#include "animation_swap_request.h"  // For swap_request_t
#include "sdcard_channel_impl.h"
#include "makapix_channel_impl.h"
#include "makapix_channel_utils.h"
#include "view_tracker.h"  // For view_tracker_stop()
#include "config_store.h"
#include "connectivity_state.h"
#include "p3a_state.h"
#include "sd_path.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>
#include <time.h>
#include <unistd.h>

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

// Forward declarations for animation_player functions (implemented in main)
// Using weak symbols to avoid hard dependency on main component
extern esp_err_t animation_player_request_swap(const swap_request_t *request) __attribute__((weak));
extern void animation_player_display_message(const char *title, const char *body) __attribute__((weak));

// ============================================================================
// Helpers - Display Name
// ============================================================================

/**
 * @brief Get user-friendly display name from channel_id
 */
static void ps_get_display_name(const char *channel_id, char *out_name, size_t max_len)
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

// ============================================================================
// Global State
// ============================================================================

static ps_state_t s_state = {0};

ps_state_t *ps_get_state(void)
{
    return &s_state;
}

// ============================================================================
// Utilities
// ============================================================================

static bool file_exists(const char *path)
{
    if (!path || path[0] == '\0') return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

static bool has_404_marker(const char *filepath)
{
    if (!filepath || filepath[0] == '\0') return false;

    // Check for {filepath}.404 marker file
    char marker_path[264];
    int ret = snprintf(marker_path, sizeof(marker_path), "%s.404", filepath);
    if (ret < 0 || ret >= (int)sizeof(marker_path)) {
        return false;
    }

    struct stat st;
    return (stat(marker_path, &st) == 0);
}

static asset_type_t get_asset_type_from_filepath(const char *filepath)
{
    if (!filepath) return ASSET_TYPE_WEBP;

    const char *ext = strrchr(filepath, '.');
    if (!ext) return ASSET_TYPE_WEBP;

    if (strcasecmp(ext, ".webp") == 0) return ASSET_TYPE_WEBP;
    if (strcasecmp(ext, ".gif") == 0) return ASSET_TYPE_GIF;
    if (strcasecmp(ext, ".png") == 0) return ASSET_TYPE_PNG;
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return ASSET_TYPE_JPEG;

    return ASSET_TYPE_WEBP;
}

static int ps_ext_index_from_filepath(const char *filepath)
{
    if (!filepath) return 0;
    const char *ext = strrchr(filepath, '.');
    if (!ext) return 0;
    if (strcasecmp(ext, ".webp") == 0) return 0;
    if (strcasecmp(ext, ".gif") == 0) return 1;
    if (strcasecmp(ext, ".png") == 0) return 2;
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return 3;
    return 0;
}

static esp_err_t ps_storage_key_sha256(const char *storage_key, uint8_t out_sha256[32])
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
// Channel Loading
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
// Swap Request
// ============================================================================

static esp_err_t prepare_and_request_swap(const ps_artwork_t *artwork)
{
    if (!artwork || !file_exists(artwork->filepath)) {
        return ESP_ERR_NOT_FOUND;
    }

    swap_request_t request = {0};
    strlcpy(request.filepath, artwork->filepath, sizeof(request.filepath));
    request.type = artwork->type;
    request.post_id = artwork->post_id;

    // Dwell time: user override > artwork dwell
    if (s_state.dwell_time_seconds > 0) {
        request.dwell_time_ms = s_state.dwell_time_seconds * 1000;
    } else if (artwork->dwell_time_ms > 0) {
        request.dwell_time_ms = artwork->dwell_time_ms;
    } else {
        request.dwell_time_ms = config_store_get_dwell_time();
    }

    request.is_live_mode = false;
    request.start_time_ms = 0;
    request.start_frame = 0;

    if (animation_player_request_swap) {
        esp_err_t err = animation_player_request_swap(&request);
        if (err == ESP_OK) {
            // Update file mtime for LRU tracking
            time_t now = time(NULL);
            struct utimbuf times = { now, now };
            utime(artwork->filepath, &times);
        }
        return err;
    } else {
        ESP_LOGW(TAG, "animation_player_request_swap not available");
        return ESP_ERR_NOT_SUPPORTED;
    }
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
    for (size_t i = 0; i < s_state.channel_count; i++) {
        if (s_state.channels[i].entries) {
            free(s_state.channels[i].entries);
            s_state.channels[i].entries = NULL;
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
// Channel Configuration
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
    for (size_t i = 0; i < s_state.channel_count; i++) {
        if (s_state.channels[i].entries) {
            free(s_state.channels[i].entries);
            s_state.channels[i].entries = NULL;
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
// Scheduler Commands
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
 * - USER: "user:{sqid}" -> "user:uvz"
 * - HASHTAG: "hashtag:{tag}" -> "hashtag:sunset"
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
            snprintf(out_id, max_len, "user:%s", sanitized);
            break;

        case PS_CHANNEL_TYPE_HASHTAG:
            ps_sanitize_identifier(spec->identifier, sanitized, sizeof(sanitized));
            snprintf(out_id, max_len, "hashtag:%s", sanitized);
            break;

        case PS_CHANNEL_TYPE_SDCARD:
            snprintf(out_id, max_len, "sdcard");
            break;

        default:
            snprintf(out_id, max_len, "unknown");
            break;
    }
}

// Forward declarations for cache loading functions
static esp_err_t ps_load_sdcard_cache(ps_channel_state_t *ch);
static esp_err_t ps_load_makapix_cache(ps_channel_state_t *ch);

/**
 * @brief Build cache file path for a channel
 */
static void ps_build_cache_path(const char *channel_id, char *out_path, size_t max_len)
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
    // SD card channels use raw binary format (no LAi needed - files are always local)
    if (ch->type == PS_CHANNEL_TYPE_SDCARD) {
        return ps_load_sdcard_cache(ch);
    }

    // Makapix channels use channel_cache module for LAi persistence
    return ps_load_makapix_cache(ch);
}

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
        ch->available_indices = NULL;
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

    // Alias data from cache into channel state (for compatibility with existing code)
    ch->entries = ch->cache->entries;
    ch->entry_count = ch->cache->entry_count;
    ch->available_indices = ch->cache->available_indices;
    ch->available_count = ch->cache->available_count;

    ch->cache_loaded = true;
    ch->active = (ch->available_count > 0);
    ch->entry_format = PS_ENTRY_FORMAT_MAKAPIX;

    ps_touch_cache_file(ch->channel_id);

    ESP_LOGI(TAG, "Channel '%s': loaded cache with %zu entries, %zu available (makapix format)",
             ch->channel_id, ch->entry_count, ch->available_count);

    return ESP_OK;
}

esp_err_t play_scheduler_execute_command(const ps_scheduler_command_t *command)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!command || command->channel_count == 0 || command->channel_count > PS_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    // Reset the periodic refresh timer so this command triggers immediate refresh
    ps_refresh_reset_timer();

    // Stop view tracking for the old channel before switching
    // This prevents view events from being sent for the wrong channel
    view_tracker_stop();

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "Executing scheduler command: %zu channel(s), exposure=%d, pick=%d",
             command->channel_count, command->exposure_mode, command->pick_mode);

    // Free old channel entries before reconfiguring
    for (size_t i = 0; i < s_state.channel_count; i++) {
        if (s_state.channels[i].entries) {
            free(s_state.channels[i].entries);
            s_state.channels[i].entries = NULL;
        }
    }

    // Store command parameters
    s_state.exposure_mode = command->exposure_mode;
    s_state.pick_mode = command->pick_mode;
    s_state.channel_count = command->channel_count;

    // Increment epoch (history is preserved)
    s_state.epoch_id++;

    // Initialize each channel
    for (size_t i = 0; i < command->channel_count; i++) {
        const ps_channel_spec_t *spec = &command->channels[i];
        ps_channel_state_t *ch = &s_state.channels[i];

        // Build channel_id from spec
        ps_build_channel_id(spec, ch->channel_id, sizeof(ch->channel_id));
        ch->type = spec->type;

        // Reset SWRR state
        ch->credit = 0;
        ch->weight = spec->weight;  // Will be recalculated after cache load

        // Reset pick state
        ch->cursor = 0;
        ps_prng_seed(&ch->pick_rng_state, s_state.global_seed ^ (uint32_t)i ^ s_state.epoch_id);

        // Clear legacy handle
        ch->handle = NULL;

        // Reset refresh state
        ch->refresh_pending = true;  // Queue for background refresh
        ch->refresh_in_progress = false;
        ch->total_count = 0;
        ch->recent_count = 0;

        // Load existing cache if available
        ps_load_channel_cache(ch);

        ESP_LOGD(TAG, "Channel[%zu]: id='%s', type=%d, weight=%lu, active=%d, entries=%zu",
                 i, ch->channel_id, ch->type, (unsigned long)ch->weight,
                 ch->active, ch->entry_count);
    }

    // Calculate SWRR weights
    ps_swrr_calculate_weights(&s_state);

    // Store first channel as "current" for status display
    if (command->channel_count > 0) {
        strlcpy(s_state.current_channel_id, s_state.channels[0].channel_id,
                sizeof(s_state.current_channel_id));

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
        }
    }

    // Signal background refresh task to process pending channels
    ps_refresh_signal_work();

    // Update Download Manager with new channel list for round-robin downloading
    extern void download_manager_set_channels(const char **channel_ids, size_t count);
    extern void download_manager_reset_playback_initiated(void);
    const char *channel_ids[PS_MAX_CHANNELS];
    for (size_t i = 0; i < command->channel_count; i++) {
        channel_ids[i] = s_state.channels[i].channel_id;
    }
    download_manager_set_channels(channel_ids, command->channel_count);

    // Reset playback_initiated so download manager can trigger playback for new channel
    download_manager_reset_playback_initiated();

    // Check if any channel has entries we can play immediately
    bool has_entries = false;
    char first_channel_display_name[64] = "Channel";
    for (size_t i = 0; i < s_state.channel_count; i++) {
        if (s_state.channels[i].active && s_state.channels[i].entry_count > 0) {
            has_entries = true;
            break;
        }
    }
    // Get first channel's display name for UI
    if (s_state.channel_count > 0) {
        ps_get_display_name(s_state.channels[0].channel_id, first_channel_display_name, sizeof(first_channel_display_name));
    }

    xSemaphoreGive(s_state.mutex);

    // Only trigger initial playback if we have entries
    // Otherwise, let download manager trigger it when first file is available
    if (has_entries) {
        return play_scheduler_next(NULL);
    } else {
        ESP_LOGI(TAG, "No cached entries yet - waiting for refresh/download");

        // Show loading state to user while waiting for refresh/download
        // But only if we have WiFi connectivity (no point showing loading in AP mode)
        if (connectivity_state_has_wifi()) {
            extern void p3a_render_set_channel_message(const char *channel_name, int msg_type,
                                                       int progress_percent, const char *detail);
            p3a_render_set_channel_message(first_channel_display_name, 1 /* P3A_CHANNEL_MSG_LOADING */, -1,
                                            "Loading channel...");
        }
        return ESP_OK;
    }
}

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
// Download Integration (decoupled - Download Manager owns its own state)
// ============================================================================

// Download Manager now has its own cursors and round-robin logic.
// See download_manager_set_channels() for configuration.
// No longer using lookahead-based prefetch.

// ============================================================================
// Navigation
// ============================================================================

esp_err_t play_scheduler_next(ps_artwork_t *out_artwork)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    esp_err_t result = ESP_OK;
    ps_artwork_t artwork;
    bool found = false;

    // If walking forward through history, return from history
    if (ps_history_can_go_forward(&s_state)) {
        found = ps_history_go_forward(&s_state, &artwork);
    }

    if (!found) {
        // Compute fresh: pick next available artwork using availability masking
        // This iterates through channel entries, skipping files that don't exist
        found = ps_pick_next_available(&s_state, &artwork);
        if (found) {
            ps_history_push(&s_state, &artwork);
            s_state.last_played_id = artwork.artwork_id;
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "No artwork available (cold start or all channels exhausted)");
        result = ESP_ERR_NOT_FOUND;

        // Check if we should display an error message
        // Don't show messages if:
        // - Not in animation playback state (provisioning, OTA, PICO-8 streaming)
        // - PICO-8 mode is active
        // - Animation is already playing
        p3a_state_t current_state = p3a_state_get();
        if (current_state != P3A_STATE_ANIMATION_PLAYBACK) {
            // Not in animation playback mode - skip message display
            ESP_LOGD(TAG, "Skipping error message: not in animation playback state (state=%d)", current_state);
        } else {
        extern bool playback_controller_is_pico8_active(void) __attribute__((weak));
        extern bool animation_player_is_animation_ready(void) __attribute__((weak));

        bool pico8_active = playback_controller_is_pico8_active && playback_controller_is_pico8_active();
        bool animation_playing = animation_player_is_animation_ready && animation_player_is_animation_ready();

        if (pico8_active || animation_playing) {
            // Don't show error message - something else is displaying content
            ESP_LOGD(TAG, "Skipping error message: pico8=%d, animation=%d", pico8_active, animation_playing);
        } else {
            // Display appropriate message based on state
            // Priority: refresh in progress > downloading > no files
            bool any_refreshing = false;
            for (size_t i = 0; i < s_state.channel_count; i++) {
                if (s_state.channels[i].refresh_async_pending || s_state.channels[i].refresh_in_progress) {
                    any_refreshing = true;
                    break;
                }
            }

            extern void p3a_render_set_channel_message(const char *channel_name, int msg_type,
                                                       int progress_percent, const char *detail);

            // Get display name for first channel
            char display_name[64] = "Channel";
            if (s_state.channel_count > 0) {
                ps_get_display_name(s_state.channels[0].channel_id, display_name, sizeof(display_name));
            }

            // Only show loading/downloading messages if we have WiFi connectivity
            if (connectivity_state_has_wifi()) {
                if (any_refreshing) {
                    // Channel is still refreshing - show loading message
                    p3a_render_set_channel_message(display_name, 1 /* P3A_CHANNEL_MSG_LOADING */, -1,
                                                    "Updating channel index...");
                } else {
                    // Check if download manager is actively downloading
                    extern bool download_manager_is_busy(void);
                    if (download_manager_is_busy()) {
                        p3a_render_set_channel_message(display_name, 2 /* P3A_CHANNEL_MSG_DOWNLOADING */, -1,
                                                        "Downloading artwork...");
                    } else {
                        // No refresh, no download - truly no files available
                        if (animation_player_display_message) {
                            animation_player_display_message("No Artworks", "No playable files available");
                        }
                    }
                }
            } else {
                // No WiFi - can't load channels from Makapix
                if (animation_player_display_message) {
                    animation_player_display_message("No Artworks", "No playable files available");
                }
            }
        }
        }  // Close the "if (current_state == P3A_STATE_ANIMATION_PLAYBACK)" else block
    } else {
        // Request swap
        result = prepare_and_request_swap(&artwork);

        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Swap request failed: %s", esp_err_to_name(result));
        }

        if (out_artwork) {
            memcpy(out_artwork, &artwork, sizeof(ps_artwork_t));
        }
    }

    xSemaphoreGive(s_state.mutex);

    return result;
}

esp_err_t play_scheduler_prev(ps_artwork_t *out_artwork)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    esp_err_t result = ESP_OK;
    ps_artwork_t artwork;

    if (!ps_history_can_go_back(&s_state)) {
        ESP_LOGD(TAG, "Cannot go back - at history start");
        result = ESP_ERR_NOT_FOUND;
    } else if (!ps_history_go_back(&s_state, &artwork)) {
        result = ESP_ERR_NOT_FOUND;
    } else {
        // Request swap
        result = prepare_and_request_swap(&artwork);

        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Swap request failed: %s", esp_err_to_name(result));
        }

        if (out_artwork) {
            memcpy(out_artwork, &artwork, sizeof(ps_artwork_t));
        }
    }

    xSemaphoreGive(s_state.mutex);

    return result;
}

esp_err_t play_scheduler_peek_next(ps_artwork_t *out_artwork)
{
    if (!s_state.initialized || !out_artwork) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    // Peek at what ps_pick_next_available would return without modifying state
    bool found = ps_peek_next_available(&s_state, out_artwork);

    xSemaphoreGive(s_state.mutex);

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t play_scheduler_current(ps_artwork_t *out_artwork)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    bool found = ps_history_get_current(&s_state, out_artwork);

    xSemaphoreGive(s_state.mutex);

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// ============================================================================
// NAE
// ============================================================================

void play_scheduler_set_nae_enabled(bool enable)
{
    if (!s_state.initialized) return;
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.nae_enabled = enable;
    xSemaphoreGive(s_state.mutex);
}

bool play_scheduler_is_nae_enabled(void)
{
    return s_state.nae_enabled;
}

void play_scheduler_nae_insert(const ps_artwork_t *artwork)
{
    if (!s_state.initialized || !artwork) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    ps_nae_insert(&s_state, artwork);
    xSemaphoreGive(s_state.mutex);
}

// ============================================================================
// Timer & Dwell
// ============================================================================

void play_scheduler_set_dwell_time(uint32_t seconds)
{
    if (!s_state.initialized) return;
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.dwell_time_seconds = seconds;
    ps_timer_reset(&s_state);
    xSemaphoreGive(s_state.mutex);
    ESP_LOGI(TAG, "Dwell time set to %lu seconds", (unsigned long)seconds);
}

uint32_t play_scheduler_get_dwell_time(void)
{
    return s_state.dwell_time_seconds;
}

void play_scheduler_reset_timer(void)
{
    if (!s_state.initialized) return;
    ps_timer_reset(&s_state);
}

// ============================================================================
// Touch Events
// ============================================================================

void play_scheduler_touch_next(void)
{
    s_state.touch_next = true;
}

void play_scheduler_touch_back(void)
{
    s_state.touch_back = true;
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

// ============================================================================
// LAi (Locally Available index) Integration
// ============================================================================

/**
 * @brief Find channel index by channel_id
 */
static int ps_find_channel_index(const char *channel_id)
{
    if (!channel_id) return -1;

    for (size_t i = 0; i < s_state.channel_count; i++) {
        if (strcmp(s_state.channels[i].channel_id, channel_id) == 0) {
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
            // Update aliased count (entries/indices are aliased, count needs sync)
            ch->available_count = ch->cache->available_count;
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

void play_scheduler_on_download_complete(const char *channel_id, const char *storage_key)
{
    if (!s_state.initialized || !channel_id || !storage_key) {
        return;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    int ch_idx = ps_find_channel_index(channel_id);
    if (ch_idx < 0) {
        ESP_LOGD(TAG, "Download complete for unknown channel: %s", channel_id);
        xSemaphoreGive(s_state.mutex);
        return;
    }

    ps_channel_state_t *ch = &s_state.channels[ch_idx];

    // Find the Ci index for this storage_key
    uint32_t ci_index = ps_find_ci_by_storage_key(ch, storage_key);
    if (ci_index == UINT32_MAX) {
        // Entry not found in current in-memory cache - the cache file may have been
        // updated by the refresh task. Reload the cache from disk and try again.
        ESP_LOGI(TAG, "Entry not in cache, reloading channel '%s' from disk", channel_id);
        esp_err_t reload_err = ps_load_channel_cache(ch);
        if (reload_err == ESP_OK) {
            // Recalculate SWRR weights after cache reload
            ps_swrr_calculate_weights(&s_state);
            ci_index = ps_find_ci_by_storage_key(ch, storage_key);
        }
        
        if (ci_index == UINT32_MAX) {
            ESP_LOGD(TAG, "Downloaded file still not in Ci after reload: %s", storage_key);
            xSemaphoreGive(s_state.mutex);
            return;
        }
        
        // After reload, LAi is already rebuilt with currently-available files
        // So the downloaded file should already be in LAi
        ESP_LOGI(TAG, "Cache reloaded, entry found at ci=%lu, LAi has %zu entries",
                 (unsigned long)ci_index, ch->available_count);
        
        // Check for zero-to-one transition and trigger playback if needed
        size_t total_available = 0;
        for (size_t i = 0; i < s_state.channel_count; i++) {
            total_available += s_state.channels[i].available_count;
        }
        
        if (total_available > 0) {
            ESP_LOGI(TAG, "After cache reload - triggering playback (%zu total available)", total_available);
            xSemaphoreGive(s_state.mutex);
            play_scheduler_next(NULL);
            return;
        }
        
        xSemaphoreGive(s_state.mutex);
        return;
    }

    // Track if this is a zero-to-one transition
    size_t prev_total_available = 0;
    for (size_t i = 0; i < s_state.channel_count; i++) {
        prev_total_available += s_state.channels[i].available_count;
    }

    // Add to LAi
    if (ps_lai_add(ch, ci_index)) {
        ESP_LOGI(TAG, "LAi add: ch='%s' ci=%lu, now %zu available",
                 channel_id, (unsigned long)ci_index, ch->available_count);

        // Check for zero-to-one transition
        if (prev_total_available == 0 && ch->available_count > 0) {
            ESP_LOGI(TAG, "Zero-to-one transition - triggering playback");
            xSemaphoreGive(s_state.mutex);

            // Trigger playback outside mutex to avoid deadlock
            play_scheduler_next(NULL);
            return;
        }
    }

    xSemaphoreGive(s_state.mutex);
}

void play_scheduler_on_load_failed(const char *storage_key, const char *channel_id, const char *reason)
{
    if (!s_state.initialized || !storage_key) {
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
        // Build path: {vault}/{sha[0:2]}/{sha[2:4]}/{sha[4:6]}/{storage_key}.{ext}
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
        xSemaphoreTake(s_state.mutex, portMAX_DELAY);

        int ch_idx = ps_find_channel_index(channel_id);
        if (ch_idx >= 0) {
            ps_channel_state_t *ch = &s_state.channels[ch_idx];
            uint32_t ci_index = ps_find_ci_by_storage_key(ch, storage_key);
            if (ci_index != UINT32_MAX && ps_lai_remove(ch, ci_index)) {
                ESP_LOGI(TAG, "LAi remove: ch='%s' ci=%lu, now %zu available",
                         channel_id, (unsigned long)ci_index, ch->available_count);
            }
        }

        // Check if we need to try another artwork
        size_t total_available = 0;
        for (size_t i = 0; i < s_state.channel_count; i++) {
            total_available += s_state.channels[i].available_count;
        }

        xSemaphoreGive(s_state.mutex);

        // Try to pick another artwork if any available
        if (total_available > 0) {
            ESP_LOGI(TAG, "Trying another artwork after load failure");
            play_scheduler_next(NULL);
        } else {
            ESP_LOGW(TAG, "No artworks available after load failure");
            // Show appropriate message (only if we have WiFi)
            if (connectivity_state_has_wifi()) {
                extern void p3a_render_set_channel_message(const char *channel_name, int msg_type,
                                                           int progress_percent, const char *detail);
                extern bool download_manager_is_busy(void);
                
                // Get display name for first channel
                char ch_display_name[64] = "Channel";
                xSemaphoreTake(s_state.mutex, portMAX_DELAY);
                if (s_state.channel_count > 0) {
                    ps_get_display_name(s_state.channels[0].channel_id, ch_display_name, sizeof(ch_display_name));
                }
                xSemaphoreGive(s_state.mutex);
                
                if (download_manager_is_busy()) {
                    p3a_render_set_channel_message(ch_display_name, 2 /* P3A_CHANNEL_MSG_DOWNLOADING */, -1,
                                                    "Downloading artwork...");
                } else {
                    p3a_render_set_channel_message(NULL, 0, -1, NULL);
                }
            }
        }
    }
}

size_t play_scheduler_get_total_available(void)
{
    if (!s_state.initialized) {
        return 0;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    size_t total = 0;
    for (size_t i = 0; i < s_state.channel_count; i++) {
        total += s_state.channels[i].available_count;
    }

    xSemaphoreGive(s_state.mutex);

    return total;
}

void play_scheduler_get_channel_stats(const char *channel_id, size_t *out_total, size_t *out_cached)
{
    if (out_total) *out_total = 0;
    if (out_cached) *out_cached = 0;

    if (!channel_id || !s_state.initialized) {
        return;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    // Find channel by ID
    for (size_t i = 0; i < s_state.channel_count; i++) {
        if (strcmp(s_state.channels[i].channel_id, channel_id) == 0) {
            if (out_total) *out_total = s_state.channels[i].entry_count;
            if (out_cached) *out_cached = s_state.channels[i].available_count;
            break;
        }
    }

    xSemaphoreGive(s_state.mutex);
}

