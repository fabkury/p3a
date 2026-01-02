// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler.c
 * @brief Play Scheduler - Core implementation
 *
 * This file implements the main Play Scheduler logic including:
 * - Initialization and deinitialization
 * - Channel configuration and loading
 * - Navigation (next/prev/current)
 * - Generation of lookahead batches
 * - Integration with animation_player
 *
 * @see docs/play-scheduler/SPECIFICATION.md
 */

#include "play_scheduler.h"
#include "play_scheduler_internal.h"
#include "channel_interface.h"
#include "channel_player.h"  // For swap_request_t
#include "sdcard_channel_impl.h"
#include "makapix_channel_impl.h"
#include "config_store.h"
#include "p3a_state.h"
#include "sd_path.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "play_scheduler";

// Forward declarations for animation_player functions (implemented in main)
// Using weak symbols to avoid hard dependency on main component
extern esp_err_t animation_player_request_swap(const swap_request_t *request) __attribute__((weak));
extern void animation_player_display_message(const char *title, const char *body) __attribute__((weak));

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
// Generation
// ============================================================================

void ps_generate_batch(ps_state_t *state)
{
    if (!state || state->channel_count == 0) {
        return;
    }

    ESP_LOGD(TAG, "Generating batch of %d items", PS_LOOKAHEAD_SIZE);

    for (int b = 0; b < PS_LOOKAHEAD_SIZE; b++) {
        ps_artwork_t candidate;
        bool found = false;

        // For single-channel mode (N=1), just pick from the first active channel
        // Multi-channel SWRR will be added in Phase 3
        for (size_t i = 0; i < state->channel_count && !found; i++) {
            if (!state->channels[i].active) continue;

            found = ps_pick_artwork(state, i, &candidate);
        }

        if (found) {
            // Check immediate repeat
            if (candidate.artwork_id == state->last_played_id) {
                // Try once more
                for (size_t i = 0; i < state->channel_count; i++) {
                    if (!state->channels[i].active) continue;
                    if (ps_pick_artwork(state, i, &candidate)) {
                        break;
                    }
                }
            }

            ps_lookahead_push(state, &candidate);
        }
    }

    ESP_LOGD(TAG, "Generation complete, lookahead now has %zu items", state->lookahead_count);
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
        return animation_player_request_swap(&request);
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

    ESP_LOGI(TAG, "Initializing Play Scheduler (H=%d, L=%d)",
             PS_HISTORY_SIZE, PS_LOOKAHEAD_SIZE);

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

    // Allocate lookahead buffer
    s_state.lookahead = calloc(PS_LOOKAHEAD_SIZE, sizeof(ps_artwork_t));
    if (!s_state.lookahead) {
        ESP_LOGE(TAG, "Failed to allocate lookahead buffer");
        free(s_state.history);
        s_state.history = NULL;
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Initialize buffers
    ps_history_init(&s_state);
    ps_lookahead_init(&s_state);

    // Initialize state
    s_state.nae_count = 0;
    s_state.nae_enabled = true;
    s_state.epoch_id = 0;
    s_state.last_played_id = -1;
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

    // Free buffers
    if (s_state.history) {
        free(s_state.history);
        s_state.history = NULL;
    }
    if (s_state.lookahead) {
        free(s_state.lookahead);
        s_state.lookahead = NULL;
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
    ps_lookahead_clear(&s_state);
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
 */
static esp_err_t ps_load_channel_cache(ps_channel_state_t *ch)
{
    char cache_path[512];
    ps_build_cache_path(ch->channel_id, cache_path, sizeof(cache_path));

    struct stat st;
    if (stat(cache_path, &st) != 0) {
        // Cache doesn't exist yet
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->active = false;
        ch->weight = 0;  // weight=0 until cache arrives
        ESP_LOGD(TAG, "Channel '%s': no cache file", ch->channel_id);
        return ESP_ERR_NOT_FOUND;
    }

    // Validate file size
    if (st.st_size <= 0 || st.st_size % 64 != 0) {
        ESP_LOGW(TAG, "Channel '%s': invalid cache file size %ld", ch->channel_id, (long)st.st_size);
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->active = false;
        ch->weight = 0;
        return ESP_ERR_INVALID_SIZE;
    }

    ch->entry_count = st.st_size / 64;
    ch->cache_loaded = true;
    ch->active = (ch->entry_count > 0);

    // Touch cache file for LRU tracking
    ps_touch_cache_file(ch->channel_id);

    ESP_LOGI(TAG, "Channel '%s': loaded cache with %zu entries", ch->channel_id, ch->entry_count);

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

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "Executing scheduler command: %zu channel(s), exposure=%d, pick=%d",
             command->channel_count, command->exposure_mode, command->pick_mode);

    // Store command parameters
    s_state.exposure_mode = command->exposure_mode;
    s_state.pick_mode = command->pick_mode;
    s_state.channel_count = command->channel_count;

    // Flush lookahead (but preserve history!)
    ps_lookahead_clear(&s_state);

    // Increment epoch
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
    }

    // Signal background refresh task to process pending channels
    ps_refresh_signal_work();

    xSemaphoreGive(s_state.mutex);

    // Trigger initial playback
    return play_scheduler_next(NULL);
}

esp_err_t play_scheduler_play_named_channel(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_named_channel: %s", name);

    ps_scheduler_command_t cmd = {0};
    cmd.channel_count = 1;
    cmd.exposure_mode = PS_EXPOSURE_EQUAL;
    cmd.pick_mode = PS_PICK_RECENCY;

    // Determine channel type
    if (strcmp(name, "sdcard") == 0) {
        cmd.channels[0].type = PS_CHANNEL_TYPE_SDCARD;
        strlcpy(cmd.channels[0].name, "sdcard", sizeof(cmd.channels[0].name));
    } else {
        cmd.channels[0].type = PS_CHANNEL_TYPE_NAMED;
        strlcpy(cmd.channels[0].name, name, sizeof(cmd.channels[0].name));
    }
    cmd.channels[0].weight = 1;

    return play_scheduler_execute_command(&cmd);
}

esp_err_t play_scheduler_play_user_channel(const char *user_sqid)
{
    if (!user_sqid) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_user_channel: %s", user_sqid);

    ps_scheduler_command_t cmd = {0};
    cmd.channel_count = 1;
    cmd.exposure_mode = PS_EXPOSURE_EQUAL;
    cmd.pick_mode = PS_PICK_RECENCY;

    cmd.channels[0].type = PS_CHANNEL_TYPE_USER;
    strlcpy(cmd.channels[0].name, "user", sizeof(cmd.channels[0].name));
    strlcpy(cmd.channels[0].identifier, user_sqid, sizeof(cmd.channels[0].identifier));
    cmd.channels[0].weight = 1;

    return play_scheduler_execute_command(&cmd);
}

esp_err_t play_scheduler_play_hashtag_channel(const char *hashtag)
{
    if (!hashtag) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_hashtag_channel: %s", hashtag);

    ps_scheduler_command_t cmd = {0};
    cmd.channel_count = 1;
    cmd.exposure_mode = PS_EXPOSURE_EQUAL;
    cmd.pick_mode = PS_PICK_RECENCY;

    cmd.channels[0].type = PS_CHANNEL_TYPE_HASHTAG;
    strlcpy(cmd.channels[0].name, "hashtag", sizeof(cmd.channels[0].name));
    strlcpy(cmd.channels[0].identifier, hashtag, sizeof(cmd.channels[0].identifier));
    cmd.channels[0].weight = 1;

    return play_scheduler_execute_command(&cmd);
}

esp_err_t play_scheduler_refresh_sdcard_cache(void)
{
    ESP_LOGI(TAG, "Refreshing SD card cache");
    return ps_build_sdcard_index();
}

// ============================================================================
// Download Integration
// ============================================================================

void play_scheduler_signal_lookahead_changed(void)
{
    // Signal download manager that lookahead has changed
    extern void download_manager_signal_work_available(void);
    download_manager_signal_work_available();
}

esp_err_t play_scheduler_get_next_prefetch(ps_prefetch_request_t *out_request)
{
    if (!s_state.initialized || !out_request) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    esp_err_t result = ESP_ERR_NOT_FOUND;
    ps_artwork_t artwork;

    // Scan lookahead for first item needing download
    size_t count = ps_lookahead_count(&s_state);
    for (size_t i = 0; i < count; i++) {
        if (!ps_lookahead_peek(&s_state, i, &artwork)) {
            continue;
        }

        // Skip if file exists
        if (file_exists(artwork.filepath)) {
            continue;
        }

        // Skip if 404 marker exists
        if (has_404_marker(artwork.filepath)) {
            continue;
        }

        // This item needs download
        memset(out_request, 0, sizeof(*out_request));
        strlcpy(out_request->storage_key, artwork.storage_key, sizeof(out_request->storage_key));
        strlcpy(out_request->filepath, artwork.filepath, sizeof(out_request->filepath));

        // Get channel ID from artwork
        if (artwork.channel_index < s_state.channel_count) {
            strlcpy(out_request->channel_id,
                    s_state.channels[artwork.channel_index].channel_id,
                    sizeof(out_request->channel_id));
        }

        // Build artwork URL from storage key
        uint8_t sha256[32] = {0};
        static const char *s_ext_strings[] = { ".webp", ".gif", ".png", ".jpg" };
        if (ps_storage_key_sha256(out_request->storage_key, sha256) == ESP_OK) {
            int ext = ps_ext_index_from_filepath(out_request->filepath);
            snprintf(out_request->art_url, sizeof(out_request->art_url),
                     "https://%s/api/vault/%02x/%02x/%02x/%s%s",
                     CONFIG_MAKAPIX_CLUB_HOST,
                     (unsigned int)sha256[0], (unsigned int)sha256[1], (unsigned int)sha256[2],
                     out_request->storage_key,
                     s_ext_strings[(ext >= 0 && ext <= 3) ? ext : 0]);
        } else {
            out_request->art_url[0] = '\0';
        }

        result = ESP_OK;
        break;
    }

    xSemaphoreGive(s_state.mutex);
    return result;
}

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
    bool have_artwork = false;

    // If walking forward through history, return from history
    if (ps_history_can_go_forward(&s_state)) {
        if (ps_history_go_forward(&s_state, &artwork)) {
            have_artwork = true;
        }
    }

    if (!have_artwork) {
        // Lenient skip loop: find a playable item in lookahead
        size_t max_skip = PS_LOOKAHEAD_SIZE;
        size_t skipped = 0;
        bool generated = false;

        while (!have_artwork && skipped < max_skip) {
            // Generate more if needed
            if (ps_lookahead_is_low(&s_state)) {
                ps_generate_batch(&s_state);
                generated = true;
            }

            // Peek at head item
            if (!ps_lookahead_peek(&s_state, 0, &artwork)) {
                // Lookahead empty
                break;
            }

            // Check if file exists locally
            if (file_exists(artwork.filepath)) {
                // File is available - use it
                ps_lookahead_pop(&s_state, &artwork);
                ps_history_push(&s_state, &artwork);
                s_state.last_played_id = artwork.artwork_id;
                have_artwork = true;
            } else if (has_404_marker(artwork.filepath)) {
                // Permanently unavailable - remove from lookahead
                ESP_LOGD(TAG, "Removing 404'd item: %s", artwork.filepath);
                ps_lookahead_pop(&s_state, NULL);
                // Don't increment skipped - this is a removal, not a skip
            } else {
                // File not downloaded yet - rotate to end and try next
                ESP_LOGD(TAG, "Skipping not-yet-downloaded: %s", artwork.filepath);
                ps_lookahead_rotate(&s_state);
                skipped++;
            }
        }

        // Signal download manager if we generated new items or skipped
        if (generated || skipped > 0) {
            play_scheduler_signal_lookahead_changed();
        }
    }

    if (!have_artwork) {
        ESP_LOGW(TAG, "No artwork available (skipped %zu)", PS_LOOKAHEAD_SIZE);
        result = ESP_ERR_NOT_FOUND;

        // Display error message
        if (animation_player_display_message) {
            animation_player_display_message("No Artworks", "No playable files available");
        }
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

esp_err_t play_scheduler_peek(
    size_t n,
    ps_artwork_t *out_artworks,
    size_t *out_count)
{
    if (!s_state.initialized) {
        if (out_count) *out_count = 0;
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    // peek() does NOT trigger generation per spec
    size_t count = ps_lookahead_peek_many(&s_state, n, out_artworks);

    if (out_count) *out_count = count;

    xSemaphoreGive(s_state.mutex);

    return ESP_OK;
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
    out_stats->lookahead_count = s_state.lookahead_count;
    out_stats->nae_pool_count = s_state.nae_count;
    out_stats->epoch_id = s_state.epoch_id;
    out_stats->current_channel_id = s_state.channel_count > 0
        ? s_state.current_channel_id
        : NULL;

    xSemaphoreGive(s_state.mutex);

    return ESP_OK;
}

void play_scheduler_reset(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "Resetting scheduler (epoch %lu -> %lu)",
             (unsigned long)s_state.epoch_id,
             (unsigned long)(s_state.epoch_id + 1));

    // Clear lookahead
    ps_lookahead_clear(&s_state);

    // Clear NAE pool
    ps_nae_clear(&s_state);

    // Reset per-channel state
    for (size_t i = 0; i < s_state.channel_count; i++) {
        ps_pick_reset_channel(&s_state, i);
        s_state.channels[i].credit = 0;
    }

    // Increment epoch
    s_state.epoch_id++;

    // Note: History is preserved across resets

    xSemaphoreGive(s_state.mutex);
}

