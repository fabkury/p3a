// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file channel_player.c
 * @brief Channel player with integrated navigation (Phase 2 refactor)
 * 
 * This component is the central authority for player navigation. It:
 * - Manages p/q indices and play order (absorbed from play_navigator)
 * - Validates all navigation commands before passing to animation_player
 * - Owns the auto-swap timer task
 * - Implements command gating to prevent concurrent navigation
 * - Schedules Live Mode swap_future events
 * - Controls background download priorities
 */

#include "channel_player.h"
#include "sdcard_channel_impl.h"
#include "config_store.h"
#include "swap_future.h"
#include "playlist_manager.h"
#include "download_manager.h"
#include "live_mode.h"
#include "play_navigator.h"
#include "pcg32_reversible.h"
#include "sntp_sync.h"
#include "sync_playlist.h"
#include "p3a_render.h"
#include "esp_random.h"

// Forward declarations for animation_player functions (implemented in main)
// Using weak symbols to avoid hard dependency on main component
extern esp_err_t animation_player_request_swap(const swap_request_t *request) __attribute__((weak));
extern void animation_player_display_message(const char *title, const char *body) __attribute__((weak));

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>

static const char *TAG = "channel_player";

// Play order types are defined in play_navigator.h

// ============================================================================
// INTERNAL STATE
// ============================================================================

typedef struct {
    bool initialized;
    
    // Channel management
    channel_player_source_t source_type;
    channel_handle_t current_channel;
    channel_handle_t sdcard_channel;  // Owned
    char channel_id[64];              // For download manager coordination
    
    // Navigation state (absorbed from play_navigator)
    play_order_mode_t order;
    uint32_t pe;                      // Playlist expansion (0 = infinite)
    bool randomize_playlist;          // Randomize within playlists
    bool live_mode;                   // Live Mode synchronization
    uint32_t global_seed;             // Global seed for deterministic random
    
    uint32_t p;                       // Post index in channel
    uint32_t q;                       // In-playlist artwork index
    
    // Cached post order mapping
    uint32_t *order_indices;
    size_t order_count;
    
    // Dwell time
    uint32_t dwell_time_seconds;      // Global dwell override (0 = use per-artwork)
    uint32_t channel_dwell_override_ms; // Channel-level dwell (0 = disabled)
    
    // Live mode flattened schedule
    bool live_ready;
    uint32_t live_count;
    uint32_t *live_p;
    uint32_t *live_q;
    
    // PCG32 PRNG for reversible random ordering
    pcg32_rng_t pcg_rng;
    
    // Command gating
    SemaphoreHandle_t command_mutex;
    bool command_active;
    
    // Touch event flags (lightweight signal mechanism)
    volatile bool touch_swap_next;
    volatile bool touch_swap_back;
    
    // Auto-swap timer task
    TaskHandle_t timer_task_handle;
    
    // Cached data for legacy API
    sdcard_post_t current_post;
    char filepath_buf[256];
    char name_buf[128];
} channel_player_state_t;

static channel_player_state_t s_player = {0};

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static esp_err_t rebuild_order(void);
static esp_err_t prepare_swap_request(swap_request_t *out);
static void timer_task(void *arg);
static uint64_t wall_clock_ms(void);
static esp_err_t navigate_next_internal(void);
static esp_err_t navigate_prev_internal(void);

// ============================================================================
// UTILITIES
// ============================================================================

static uint64_t wall_clock_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static uint32_t get_effective_seed(void)
{
    uint32_t effective = config_store_get_effective_seed();
    return effective ^ s_player.global_seed;
}

static bool file_exists(const char *path)
{
    if (!path) return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

static channel_order_mode_t map_play_order_to_channel_order(uint8_t play_order)
{
    switch (play_order) {
        case 1: return CHANNEL_ORDER_CREATED;
        case 2: return CHANNEL_ORDER_RANDOM;
        case 0:
        default:
            return CHANNEL_ORDER_ORIGINAL;
    }
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

// ============================================================================
// PLAY ORDER / SHUFFLE LOGIC (from play_navigator.c)
// ============================================================================

static void shuffle_indices(uint32_t *indices, size_t count, pcg32_rng_t *rng)
{
    if (!indices || count <= 1 || !rng) return;
    
    for (size_t i = count - 1; i > 0; i--) {
        uint32_t r = pcg32_next_u32(rng);
        size_t j = (size_t)(r % (uint32_t)(i + 1));
        uint32_t tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }
}

static esp_err_t rebuild_order(void)
{
    if (!s_player.current_channel) return ESP_ERR_INVALID_ARG;
    
    size_t post_count = channel_get_post_count(s_player.current_channel);
    if (post_count == 0) {
        free(s_player.order_indices);
        s_player.order_indices = NULL;
        s_player.order_count = 0;
        s_player.p = 0;
        s_player.q = 0;
        return ESP_OK;
    }
    
    uint32_t *indices = (uint32_t *)malloc(post_count * sizeof(uint32_t));
    if (!indices) return ESP_ERR_NO_MEM;
    
    for (size_t i = 0; i < post_count; i++) {
        indices[i] = (uint32_t)i;
    }
    
    if (s_player.order == PLAY_ORDER_CREATED) {
        // Sort by created_at (newest first)
        typedef struct {
            uint32_t idx;
            uint32_t created_at;
        } sort_item_t;
        
        sort_item_t *items = (sort_item_t *)malloc(post_count * sizeof(sort_item_t));
        if (!items) {
            free(indices);
            return ESP_ERR_NO_MEM;
        }
        
        for (size_t i = 0; i < post_count; i++) {
            channel_post_t post = {0};
            uint32_t created = 0;
            if (channel_get_post(s_player.current_channel, i, &post) == ESP_OK) {
                created = post.created_at;
            }
            items[i].idx = (uint32_t)i;
            items[i].created_at = created;
        }
        
        // Bubble sort (small lists)
        for (size_t i = 0; i + 1 < post_count; i++) {
            for (size_t j = 0; j + 1 < post_count - i; j++) {
                if (items[j].created_at < items[j + 1].created_at) {
                    sort_item_t tmp = items[j];
                    items[j] = items[j + 1];
                    items[j + 1] = tmp;
                }
            }
        }
        
        for (size_t i = 0; i < post_count; i++) {
            indices[i] = items[i].idx;
        }
        free(items);
        
    } else if (s_player.order == PLAY_ORDER_RANDOM) {
        // Shuffle using PCG32
        uint32_t seed = get_effective_seed();
        pcg32_seed(&s_player.pcg_rng, seed, 0);
        shuffle_indices(indices, post_count, &s_player.pcg_rng);
    }
    
    free(s_player.order_indices);
    s_player.order_indices = indices;
    s_player.order_count = post_count;
    
    return ESP_OK;
}

// ============================================================================
// PLAYLIST EXPANSION LOGIC (from play_navigator.c)
// ============================================================================

static size_t get_effective_playlist_size(playlist_metadata_t *playlist)
{
    if (!playlist) return 0;
    size_t count = (size_t)playlist->loaded_artworks;
    if (s_player.pe == 0) return count;  // Infinite
    size_t effective = (s_player.pe < count) ? s_player.pe : count;
    return effective;
}

static uint32_t playlist_map_q_to_index(playlist_metadata_t *playlist, uint32_t q)
{
    if (!playlist || playlist->loaded_artworks == 0) return 0;
    
    size_t effective = get_effective_playlist_size(playlist);
    if (effective == 0) return 0;
    
    // For now, simple linear mapping - randomization handled by play_navigator internally
    return (uint32_t)(q % effective);
}

// ============================================================================
// NAVIGATION: CURRENT, NEXT, PREV (from play_navigator.c)
// ============================================================================

static esp_err_t get_current_artwork(artwork_ref_t *out_artwork)
{
    if (!out_artwork || !s_player.current_channel) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_player.order_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    uint32_t post_idx = s_player.order_indices[s_player.p % s_player.order_count];
    
    channel_post_t post = {0};
    esp_err_t err = channel_get_post(s_player.current_channel, post_idx, &post);
    if (err != ESP_OK) {
        return err;
    }
    
    // Check if it's a playlist
    if (post.kind == CHANNEL_POST_KIND_PLAYLIST) {
        playlist_metadata_t *playlist = NULL;
        err = playlist_get(post.post_id, s_player.pe, &playlist);
        if (err != ESP_OK || !playlist || playlist->loaded_artworks == 0) {
            if (playlist) playlist_release(playlist);
            return ESP_ERR_NOT_FOUND;
        }
        
        uint32_t mapped_q = playlist_map_q_to_index(playlist, s_player.q);
        if (mapped_q >= (uint32_t)playlist->loaded_artworks) {
            playlist_release(playlist);
            return ESP_ERR_INVALID_STATE;
        }
        
        artwork_ref_t *art = &playlist->artworks[mapped_q];
        memcpy(out_artwork, art, sizeof(artwork_ref_t));
        
        // Override dwell if channel-level override is set
        if (s_player.channel_dwell_override_ms > 0) {
            out_artwork->dwell_time_ms = s_player.channel_dwell_override_ms;
        }
        
        playlist_release(playlist);
        return ESP_OK;
    }
    
    // Single artwork post
    memset(out_artwork, 0, sizeof(artwork_ref_t));
    strlcpy(out_artwork->filepath, post.u.artwork.filepath, sizeof(out_artwork->filepath));
    out_artwork->post_id = post.post_id;
    out_artwork->dwell_time_ms = (s_player.channel_dwell_override_ms > 0) ? 
                                  s_player.channel_dwell_override_ms : post.dwell_time_ms;
    out_artwork->downloaded = file_exists(post.u.artwork.filepath);
    
    return ESP_OK;
}

static esp_err_t navigate_next_internal(void)
{
    if (!s_player.current_channel || s_player.order_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    
    uint32_t post_idx = s_player.order_indices[s_player.p % s_player.order_count];
    channel_post_t post = {0};
    esp_err_t err = channel_get_post(s_player.current_channel, post_idx, &post);
    
    if (err == ESP_OK && post.kind == CHANNEL_POST_KIND_PLAYLIST) {
        playlist_metadata_t *playlist = NULL;
        err = playlist_get(post.post_id, s_player.pe, &playlist);
        if (err == ESP_OK && playlist) {
            size_t effective = get_effective_playlist_size(playlist);
            if (effective > 0 && s_player.q + 1 < effective) {
                // Advance within playlist
                s_player.q++;
                playlist_release(playlist);
                return ESP_OK;
            }
            playlist_release(playlist);
        }
    }
    
    // Advance to next post
    s_player.q = 0;
    s_player.p++;
    
    // Wrap around
    if (s_player.p >= s_player.order_count) {
        s_player.p = 0;
        // Re-shuffle if random mode
        if (s_player.order == PLAY_ORDER_RANDOM) {
            rebuild_order();
        }
    }
    
    return ESP_OK;
}

static esp_err_t navigate_prev_internal(void)
{
    if (!s_player.current_channel || s_player.order_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_player.q > 0) {
        // Go back within playlist
        s_player.q--;
        return ESP_OK;
    }
    
    // Go to previous post
    if (s_player.p == 0) {
        s_player.p = (s_player.order_count > 0) ? (s_player.order_count - 1) : 0;
    } else {
        s_player.p--;
    }
    
    // If the new post is a playlist, jump to its last artwork
    uint32_t post_idx = s_player.order_indices[s_player.p % s_player.order_count];
    channel_post_t post = {0};
    esp_err_t err = channel_get_post(s_player.current_channel, post_idx, &post);
    
    if (err == ESP_OK && post.kind == CHANNEL_POST_KIND_PLAYLIST) {
        playlist_metadata_t *playlist = NULL;
        err = playlist_get(post.post_id, s_player.pe, &playlist);
        if (err == ESP_OK && playlist) {
            size_t effective = get_effective_playlist_size(playlist);
            s_player.q = (effective > 0) ? (effective - 1) : 0;
            playlist_release(playlist);
        }
    } else {
        s_player.q = 0;
    }
    
    return ESP_OK;
}

// ============================================================================
// PRE-VALIDATION AND SWAP REQUEST PREPARATION
// ============================================================================

static esp_err_t prepare_swap_request(swap_request_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    
    memset(out, 0, sizeof(swap_request_t));
    
    // Get current artwork
    artwork_ref_t artwork = {0};
    esp_err_t err = get_current_artwork(&artwork);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get current artwork: %s", esp_err_to_name(err));
        return err;
    }
    
    // Check file exists
    if (!file_exists(artwork.filepath)) {
        // Don't log here - the caller will log a summary
        return ESP_ERR_NOT_FOUND;
    }
    
    // Fill swap request
    strlcpy(out->filepath, artwork.filepath, sizeof(out->filepath));
    out->type = get_asset_type_from_filepath(artwork.filepath);
    out->post_id = artwork.post_id;
    
    // Dwell time: user override > artwork dwell
    if (s_player.dwell_time_seconds > 0) {
        out->dwell_time_ms = s_player.dwell_time_seconds * 1000;
    } else {
        out->dwell_time_ms = artwork.dwell_time_ms;
    }
    
    out->is_live_mode = s_player.live_mode;
    out->start_time_ms = 0;  // TODO: Live Mode timing
    out->start_frame = 0;
    
    return ESP_OK;
}

// ============================================================================
// LIVE MODE INTEGRATION (event-driven swap_future scheduling)
// ============================================================================

static void schedule_next_live_swap(void)
{
    if (!s_player.live_mode) return;
    
    // TODO: Full Live Mode implementation requires:
    // 1. Build flattened schedule from current channel/order
    // 2. Calculate current sync position based on wall clock
    // 3. Determine next artwork and timing
    // 4. Schedule swap_future with precise timing
    
    ESP_LOGD(TAG, "Live Mode swap scheduling (simplified stub)");
    
    // For now, just schedule a basic swap after dwell time
    // This maintains basic functionality until full Live Mode is implemented
    uint32_t dwell_ms = s_player.dwell_time_seconds * 1000;
    if (dwell_ms == 0) dwell_ms = 10000;
    
    // In full implementation, this would use swap_future_schedule()
    // with precise wall-clock timing
}

void channel_player_notify_swap_succeeded(void)
{
    ESP_LOGD(TAG, "Swap succeeded notification");
    
    if (s_player.live_mode) {
        schedule_next_live_swap();
    }
    
    // Notify timer task to reset dwell countdown
    if (s_player.timer_task_handle) {
        xTaskNotifyGive(s_player.timer_task_handle);
    }
}

void channel_player_notify_swap_failed(esp_err_t error)
{
    ESP_LOGW(TAG, "Swap failed notification: %s", esp_err_to_name(error));
    
    if (s_player.live_mode) {
        // TODO: Live Mode recovery
        ESP_LOGE(TAG, "Live Mode recovery not yet implemented");
    }
}

// ============================================================================
// AUTO-SWAP TIMER TASK (absorbed from p3a_main.c)
// ============================================================================

static void timer_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Channel player timer task started");
    
    vTaskDelay(pdMS_TO_TICKS(1000));  // Initial delay
    
    while (true) {
        // Check for touch events
        if (s_player.touch_swap_next) {
            s_player.touch_swap_next = false;
            channel_player_swap_next();
        }
        if (s_player.touch_swap_back) {
            s_player.touch_swap_back = false;
            channel_player_swap_back();
        }
        
        // Live Mode: event-driven via swap_future
        if (s_player.live_mode) {
            // Wait for swap completion notification
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        
        // Normal mode: dwell-based auto-swap
        uint32_t dwell_ms = s_player.dwell_time_seconds * 1000;
        if (dwell_ms == 0) dwell_ms = 10000;  // Default 10s
        
        // Wait for dwell timeout or reset notification
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(dwell_ms));
        if (notified > 0) {
            // Timer was reset
            continue;
        }
        
        // Timeout: perform auto-swap
        ESP_LOGD(TAG, "Auto-swap timer elapsed, advancing");
        channel_player_swap_next();
    }
}

// Global function for external code to reset the auto-swap timer
void auto_swap_reset_timer(void)
{
    if (s_player.timer_task_handle) {
        xTaskNotifyGive(s_player.timer_task_handle);
    }
}

// ============================================================================
// NEW PUBLIC API (Phase 1)
// ============================================================================

esp_err_t channel_player_swap_next(void)
{
    if (!s_player.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_player.command_mutex, 0) != pdTRUE) {
        ESP_LOGD(TAG, "Command already in progress, ignoring swap_next");
        return ESP_ERR_INVALID_STATE;
    }
    
    s_player.command_active = true;
    
    // Exit Live Mode on manual swap
    if (s_player.live_mode) {
        ESP_LOGI(TAG, "Manual swap detected - exiting Live Mode");
        s_player.live_mode = false;
        swap_future_cancel();
    }
    
    // Navigate to next
    esp_err_t err = navigate_next_internal();
    if (err != ESP_OK) {
        s_player.command_active = false;
        xSemaphoreGive(s_player.command_mutex);
        return err;
    }
    
    // Prepare validated swap request
    swap_request_t request = {0};
    err = prepare_swap_request(&request);
    
    if (err == ESP_ERR_NOT_FOUND) {
        // File doesn't exist - try to find next available artwork
        // Limit: 2x the channel size to prevent infinite loops while allowing full channel scan
        size_t max_attempts = s_player.order_count * 2;
        if (max_attempts > 200) max_attempts = 200;
        if (max_attempts == 0) max_attempts = 1;  // Safety: allow at least 1 attempt
        
        size_t skips = 0;
        for (size_t attempt = 0; attempt < max_attempts; attempt++) {
            err = navigate_next_internal();
            if (err != ESP_OK) break;
            
            err = prepare_swap_request(&request);
            if (err == ESP_OK) {
                // Found a valid one
                if (skips > 0) {
                    ESP_LOGI(TAG, "Found available artwork after skipping %zu unavailable file(s)", skips);
                }
                break;
            }
            skips++;
        }
        
        if (err != ESP_OK && skips > 0) {
            ESP_LOGW(TAG, "No available artwork found after scanning %zu positions", skips);
        }
    }
    
    if (err == ESP_OK) {
        // Valid swap request - send to animation_player
        if (animation_player_request_swap) {
            err = animation_player_request_swap(&request);
        } else {
            ESP_LOGW(TAG, "animation_player_request_swap not available");
        }
    } else {
        // No valid artworks found
        ESP_LOGE(TAG, "No valid artworks available in channel");
        if (animation_player_display_message) {
            animation_player_display_message("No Artworks", "No playable files in channel");
        }
    }
    
    s_player.command_active = false;
    xSemaphoreGive(s_player.command_mutex);
    
    return ESP_OK;
}

esp_err_t channel_player_swap_back(void)
{
    if (!s_player.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_player.command_mutex, 0) != pdTRUE) {
        ESP_LOGD(TAG, "Command already in progress, ignoring swap_back");
        return ESP_ERR_INVALID_STATE;
    }
    
    s_player.command_active = true;
    
    // Exit Live Mode on manual swap
    if (s_player.live_mode) {
        ESP_LOGI(TAG, "Manual swap detected - exiting Live Mode");
        s_player.live_mode = false;
        swap_future_cancel();
    }
    
    // Navigate to previous
    esp_err_t err = navigate_prev_internal();
    if (err != ESP_OK) {
        s_player.command_active = false;
        xSemaphoreGive(s_player.command_mutex);
        return err;
    }
    
    // Prepare validated swap request
    swap_request_t request = {0};
    err = prepare_swap_request(&request);
    
    if (err == ESP_ERR_NOT_FOUND) {
        // File doesn't exist - try to find previous available artwork
        // Limit: 2x the channel size to prevent infinite loops while allowing full channel scan
        size_t max_attempts = s_player.order_count * 2;
        if (max_attempts > 200) max_attempts = 200;
        if (max_attempts == 0) max_attempts = 1;  // Safety: allow at least 1 attempt
        
        size_t skips = 0;
        for (size_t attempt = 0; attempt < max_attempts; attempt++) {
            err = navigate_prev_internal();
            if (err != ESP_OK) break;
            
            err = prepare_swap_request(&request);
            if (err == ESP_OK) {
                // Found a valid one
                if (skips > 0) {
                    ESP_LOGI(TAG, "Found available artwork after skipping %zu unavailable file(s) backwards", skips);
                }
                break;
            }
            skips++;
        }
        
        if (err != ESP_OK && skips > 0) {
            ESP_LOGW(TAG, "No available artwork found after scanning %zu positions backwards", skips);
        }
    }
    
    if (err == ESP_OK) {
        if (animation_player_request_swap) {
            err = animation_player_request_swap(&request);
        }
    } else {
        ESP_LOGE(TAG, "No valid artworks available in channel");
        if (animation_player_display_message) {
            animation_player_display_message("No Artworks", "No playable files in channel");
        }
    }
    
    s_player.command_active = false;
    xSemaphoreGive(s_player.command_mutex);
    
    return ESP_OK;
}

esp_err_t channel_player_swap_to(uint32_t p, uint32_t q)
{
    if (!s_player.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!s_player.current_channel) {
        ESP_LOGE(TAG, "swap_to: No channel loaded");
        return ESP_ERR_INVALID_STATE;
    }
    
    // If order_count is 0, try to rebuild (channel may have been populated since last check)
    if (s_player.order_count == 0) {
        rebuild_order();
        if (s_player.order_count == 0) {
            ESP_LOGD(TAG, "swap_to: Channel still empty after rebuild (order_count=0)");
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGI(TAG, "swap_to: Rebuilt order, now have %zu items", s_player.order_count);
    }
    
    if (xSemaphoreTake(s_player.command_mutex, 0) != pdTRUE) {
        ESP_LOGD(TAG, "Command already in progress, ignoring swap_to");
        return ESP_ERR_INVALID_STATE;
    }
    
    s_player.command_active = true;
    
    // Exit Live Mode on explicit swap_to
    if (s_player.live_mode) {
        ESP_LOGI(TAG, "swap_to detected - exiting Live Mode");
        s_player.live_mode = false;
        swap_future_cancel();
    }
    
    // Set position directly
    s_player.p = p % s_player.order_count;  // Wrap to valid range
    s_player.q = q;
    
    ESP_LOGI(TAG, "swap_to: Setting position to p=%lu, q=%lu", (unsigned long)s_player.p, (unsigned long)s_player.q);
    
    // Prepare validated swap request
    swap_request_t request = {0};
    esp_err_t err = prepare_swap_request(&request);
    
    if (err == ESP_ERR_NOT_FOUND) {
        // File doesn't exist - try to find next available artwork
        // Limit: 2x the channel size to prevent infinite loops
        size_t max_attempts = s_player.order_count * 2;
        if (max_attempts > 200) max_attempts = 200;
        if (max_attempts == 0) max_attempts = 1;
        
        size_t skips = 0;
        for (size_t attempt = 0; attempt < max_attempts; attempt++) {
            err = navigate_next_internal();
            if (err != ESP_OK) break;
            
            err = prepare_swap_request(&request);
            if (err == ESP_OK) {
                if (skips > 0) {
                    ESP_LOGI(TAG, "swap_to: Found available artwork after skipping %zu unavailable file(s)", skips);
                }
                break;
            }
            skips++;
        }
        
        if (err != ESP_OK && skips > 0) {
            ESP_LOGW(TAG, "swap_to: No available artwork found after scanning %zu positions", skips);
        }
    }
    
    if (err == ESP_OK) {
        // Valid swap request - send to animation_player
        if (animation_player_request_swap) {
            err = animation_player_request_swap(&request);
        } else {
            ESP_LOGW(TAG, "animation_player_request_swap not available");
            err = ESP_FAIL;
        }
    } else {
        // No valid artworks found
        ESP_LOGW(TAG, "swap_to: No valid artworks available at or after position p=%lu", (unsigned long)p);
    }
    
    s_player.command_active = false;
    xSemaphoreGive(s_player.command_mutex);
    
    return err;
}

bool channel_player_is_command_active(void)
{
    return s_player.command_active;
}

esp_err_t channel_player_switch_channel(p3a_channel_type_t type, const char *identifier)
{
    if (!s_player.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Switching to channel type %d (identifier: %s)", type, identifier ? identifier : "NULL");
    
    // Exit Live Mode on channel switch
    if (s_player.live_mode) {
        s_player.live_mode = false;
        swap_future_cancel();
    }
    
    // Determine target channel
    channel_handle_t target_channel = NULL;
    
    if (type == P3A_CHANNEL_SDCARD) {
        // Switch to SD card channel
        if (!s_player.sdcard_channel) {
            s_player.sdcard_channel = sdcard_channel_create("SD Card", NULL);
            if (!s_player.sdcard_channel) {
                ESP_LOGE(TAG, "Failed to create SD card channel");
                if (animation_player_display_message) {
                    animation_player_display_message("Channel Error", "Failed to create SD card channel");
                }
                return ESP_ERR_NO_MEM;
            }
        }
        target_channel = s_player.sdcard_channel;
        s_player.source_type = CHANNEL_PLAYER_SOURCE_SDCARD;
        strlcpy(s_player.channel_id, "sdcard", sizeof(s_player.channel_id));
        
    } else {
        // Makapix channel - would need to be created/loaded
        // For now, return not supported
        ESP_LOGW(TAG, "Makapix channel switching not yet fully implemented");
        if (animation_player_display_message) {
            animation_player_display_message("Not Implemented", "Makapix channel switching coming soon");
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    // Mark new channel as current immediately
    s_player.current_channel = target_channel;
    
    // Load/refresh channel
    channel_request_refresh(target_channel);
    esp_err_t err = channel_load(target_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load channel: %s", esp_err_to_name(err));
        if (animation_player_display_message) {
            animation_player_display_message("Channel Error", "Failed to load channel");
        }
        return err;
    }
    
    // Start playback
    channel_order_mode_t order = map_play_order_to_channel_order(config_store_get_play_order());
    err = channel_start_playback(target_channel, order, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start playback: %s", esp_err_to_name(err));
        if (animation_player_display_message) {
            animation_player_display_message("Channel Error", "Failed to start playback");
        }
        return err;
    }
    
    // Rebuild order
    err = rebuild_order();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to rebuild order: %s", esp_err_to_name(err));
    }
    
    // Try to find first available artwork
    swap_request_t request = {0};
    err = prepare_swap_request(&request);
    
    if (err == ESP_ERR_NOT_FOUND) {
        // No artworks available - display message but stay in channel
        ESP_LOGW(TAG, "No artworks available in channel");
        if (animation_player_display_message) {
            animation_player_display_message("Empty Channel", "No artworks available");
        }
        return ESP_OK;  // Channel switch succeeded, just empty
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to prepare swap request: %s", esp_err_to_name(err));
        if (animation_player_display_message) {
            animation_player_display_message("Channel Error", "Failed to prepare first artwork");
        }
        return err;
    }
    
    // Request swap to first artwork
    if (animation_player_request_swap) {
        err = animation_player_request_swap(&request);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to request swap: %s", esp_err_to_name(err));
        if (animation_player_display_message) {
            animation_player_display_message("Playback Error", "Failed to start playback");
        }
        return err;
    }
    
    ESP_LOGI(TAG, "Channel switch successful");
    return ESP_OK;
}

esp_err_t channel_player_set_dwell_time(uint32_t seconds)
{
    s_player.dwell_time_seconds = seconds;
    
    // Notify timer task to reset countdown
    if (s_player.timer_task_handle) {
        xTaskNotifyGive(s_player.timer_task_handle);
    }
    
    return ESP_OK;
}

uint32_t channel_player_get_dwell_time(void)
{
    return s_player.dwell_time_seconds;
}

esp_err_t channel_player_enter_live_mode(void)
{
    if (!sntp_sync_is_synchronized()) {
        ESP_LOGW(TAG, "Cannot enter Live Mode: NTP not synced");
        return ESP_ERR_INVALID_STATE;
    }
    
    s_player.live_mode = true;
    schedule_next_live_swap();
    
    ESP_LOGI(TAG, "Entered Live Mode");
    return ESP_OK;
}

void channel_player_exit_live_mode(void)
{
    if (s_player.live_mode) {
        s_player.live_mode = false;
        swap_future_cancel();
        ESP_LOGI(TAG, "Exited Live Mode");
    }
}

// ============================================================================
// LEGACY API (maintaining compatibility)
// ============================================================================

esp_err_t channel_player_init(void)
{
    // Verify that we're truly initialized (not just garbage in the flag)
    // by checking if a critical resource (command_mutex) was created
    if (s_player.initialized && s_player.command_mutex != NULL) {
        ESP_LOGW(TAG, "Channel player already initialized");
        return ESP_OK;
    }
    
    // Either not initialized or initialized flag was garbage - do full init
    memset(&s_player, 0, sizeof(s_player));
    
    s_player.source_type = CHANNEL_PLAYER_SOURCE_SDCARD;
    s_player.order = PLAY_ORDER_SERVER;
    s_player.pe = 0;  // Infinite playlist expansion
    s_player.randomize_playlist = false;
    s_player.live_mode = false;
    s_player.global_seed = esp_random();
    s_player.dwell_time_seconds = 0;  // Use per-artwork dwell
    
    s_player.command_mutex = xSemaphoreCreateMutex();
    if (!s_player.command_mutex) {
        ESP_LOGE(TAG, "Failed to create command mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Create timer task
    BaseType_t ret = xTaskCreate(
        timer_task,
        "ch_timer",
        4096,
        NULL,
        5,  // Priority
        &s_player.timer_task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create timer task");
        vSemaphoreDelete(s_player.command_mutex);
        return ESP_FAIL;
    }
    
    s_player.initialized = true;
    ESP_LOGI(TAG, "Channel player initialized");
    
    return ESP_OK;
}

void channel_player_deinit(void)
{
    if (!s_player.initialized) {
        return;
    }
    
    if (s_player.timer_task_handle) {
        vTaskDelete(s_player.timer_task_handle);
        s_player.timer_task_handle = NULL;
    }
    
    if (s_player.command_mutex) {
        vSemaphoreDelete(s_player.command_mutex);
        s_player.command_mutex = NULL;
    }
    
    if (s_player.sdcard_channel) {
        channel_destroy(s_player.sdcard_channel);
        s_player.sdcard_channel = NULL;
    }
    
    free(s_player.order_indices);
    free(s_player.live_p);
    free(s_player.live_q);
    
    s_player.current_channel = NULL;
    s_player.initialized = false;
}

esp_err_t channel_player_set_sdcard_channel_handle(channel_handle_t sdcard_channel)
{
    if (!s_player.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_player.sdcard_channel && s_player.sdcard_channel != sdcard_channel) {
        channel_destroy(s_player.sdcard_channel);
    }
    
    s_player.sdcard_channel = sdcard_channel;
    
    if (s_player.source_type == CHANNEL_PLAYER_SOURCE_SDCARD) {
        s_player.current_channel = s_player.sdcard_channel;
        rebuild_order();
    }
    
    return ESP_OK;
}

esp_err_t channel_player_load_channel(void)
{
    if (!s_player.initialized) {
        ESP_LOGE(TAG, "Channel player not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!s_player.current_channel) {
        if (!s_player.sdcard_channel) {
            s_player.sdcard_channel = sdcard_channel_create("SD Card", NULL);
            if (!s_player.sdcard_channel) {
                return ESP_ERR_NO_MEM;
            }
        }
        s_player.current_channel = s_player.sdcard_channel;
        s_player.source_type = CHANNEL_PLAYER_SOURCE_SDCARD;
    }
    
    // Request refresh (for Makapix this is async, for SD card it's sync with load)
    // We ignore the return value because refresh may be async
    (void)channel_request_refresh(s_player.current_channel);
    
    // Always call load to ensure channel data is available synchronously
    // For SD card: this may reload (inefficient but safe)
    // For Makapix: this loads from local cache while async refresh runs in background
    esp_err_t err = channel_load(s_player.current_channel);
    if (err != ESP_OK) {
        return err;
    }
    
    channel_order_mode_t order = map_play_order_to_channel_order(config_store_get_play_order());
    err = channel_start_playback(s_player.current_channel, order, NULL);
    if (err != ESP_OK) {
        return err;
    }
    
    return rebuild_order();
}

esp_err_t channel_player_get_current_item(channel_item_ref_t *out_item)
{
    if (!s_player.initialized || !out_item) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_player.current_channel) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return channel_current_item(s_player.current_channel, out_item);
}

esp_err_t channel_player_get_current_post_id(int32_t *out_post_id)
{
    if (!s_player.initialized || !out_post_id) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *out_post_id = 0;
    
    if (!s_player.current_channel) {
        return ESP_ERR_INVALID_STATE;
    }
    
    static channel_item_ref_t s_temp_item;
    esp_err_t err = channel_current_item(s_player.current_channel, &s_temp_item);
    if (err == ESP_OK) {
        *out_post_id = s_temp_item.post_id;
    }
    return err;
}

esp_err_t channel_player_get_current_post(const sdcard_post_t **out_post)
{
    if (!s_player.initialized || !out_post) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_player.current_channel) {
        return ESP_ERR_INVALID_STATE;
    }
    
    channel_item_ref_t item;
    esp_err_t err = channel_current_item(s_player.current_channel, &item);
    if (err != ESP_OK) {
        return err;
    }
    
    strlcpy(s_player.filepath_buf, item.filepath, sizeof(s_player.filepath_buf));
    
    const char *name = strrchr(item.filepath, '/');
    name = name ? (name + 1) : item.filepath;
    strlcpy(s_player.name_buf, name, sizeof(s_player.name_buf));
    
    asset_type_t type = get_asset_type_from_filepath(item.filepath);
    
    s_player.current_post.name = s_player.name_buf;
    s_player.current_post.filepath = s_player.filepath_buf;
    s_player.current_post.type = type;
    s_player.current_post.created_at = 0;
    s_player.current_post.dwell_time_ms = item.dwell_time_ms;
    s_player.current_post.healthy = true;
    
    *out_post = &s_player.current_post;
    return ESP_OK;
}

esp_err_t channel_player_advance(void)
{
    if (!s_player.initialized || !s_player.current_channel) {
        return ESP_ERR_INVALID_STATE;
    }
    
    channel_item_ref_t item;
    return channel_next_item(s_player.current_channel, &item);
}

esp_err_t channel_player_go_back(void)
{
    if (!s_player.initialized || !s_player.current_channel) {
        return ESP_ERR_INVALID_STATE;
    }
    
    channel_item_ref_t item;
    return channel_prev_item(s_player.current_channel, &item);
}

void channel_player_set_randomize(bool enable)
{
    (void)enable;
    // Deprecated
}

bool channel_player_is_randomized(void)
{
    return false;
}

size_t channel_player_get_current_position(void)
{
    if (!s_player.initialized || !s_player.current_channel) {
        return SIZE_MAX;
    }
    
    channel_stats_t stats;
    if (channel_get_stats(s_player.current_channel, &stats) != ESP_OK) {
        return SIZE_MAX;
    }
    
    return stats.current_position;
}

size_t channel_player_get_post_count(void)
{
    if (!s_player.initialized || !s_player.current_channel) {
        return 0;
    }
    
    channel_stats_t stats;
    if (channel_get_stats(s_player.current_channel, &stats) != ESP_OK) {
        return 0;
    }
    
    return stats.total_items;
}

esp_err_t channel_player_switch_to_makapix_channel(channel_handle_t makapix_channel)
{
    if (!s_player.initialized) {
        ESP_LOGE(TAG, "Channel player not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!makapix_channel) {
        ESP_LOGE(TAG, "Invalid channel handle");
        return ESP_ERR_INVALID_ARG;
    }
    
    s_player.source_type = CHANNEL_PLAYER_SOURCE_MAKAPIX;
    s_player.current_channel = makapix_channel;
    rebuild_order();
    
    // Notify timer task
    if (s_player.timer_task_handle) {
        xTaskNotifyGive(s_player.timer_task_handle);
    }
    
    return ESP_OK;
}

esp_err_t channel_player_switch_to_sdcard_channel(void)
{
    if (!s_player.initialized) {
        ESP_LOGE(TAG, "Channel player not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!s_player.sdcard_channel) {
        s_player.sdcard_channel = sdcard_channel_create("SD Card", NULL);
        if (!s_player.sdcard_channel) {
            return ESP_ERR_NO_MEM;
        }
    }
    
    s_player.source_type = CHANNEL_PLAYER_SOURCE_SDCARD;
    s_player.current_channel = s_player.sdcard_channel;
    rebuild_order();
    
    // Notify timer task
    if (s_player.timer_task_handle) {
        xTaskNotifyGive(s_player.timer_task_handle);
    }
    
    return ESP_OK;
}

channel_player_source_t channel_player_get_source_type(void)
{
    return s_player.initialized ? s_player.source_type : CHANNEL_PLAYER_SOURCE_SDCARD;
}

bool channel_player_is_live_mode_active(void)
{
    return s_player.live_mode;
}

void *channel_player_get_navigator(void)
{
    // Navigator is now internal, return NULL for compatibility
    return NULL;
}

void channel_player_clear_channel(channel_handle_t channel_to_clear)
{
    if (!s_player.initialized) {
        return;
    }
    
    if (s_player.current_channel == channel_to_clear) {
        ESP_LOGI(TAG, "Clearing current channel pointer (channel about to be destroyed)");
        s_player.current_channel = NULL;
    }
}

esp_err_t channel_player_set_play_order(uint8_t play_order)
{
    if (!s_player.initialized || !s_player.current_channel) {
        ESP_LOGW(TAG, "Cannot set play order: no active channel");
        return ESP_ERR_INVALID_STATE;
    }
    
    play_order_mode_t mode;
    switch (play_order) {
        case 1: mode = PLAY_ORDER_CREATED; break;
        case 2: mode = PLAY_ORDER_RANDOM; break;
        case 0:
        default:
            mode = PLAY_ORDER_SERVER;
            break;
    }
    
    s_player.order = mode;
    ESP_LOGI(TAG, "Hot-swapping play order to %d", (int)mode);
    return rebuild_order();
}
