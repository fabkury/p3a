// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_navigation.c
 * @brief Play Scheduler - Navigation and swap request handling
 *
 * This file implements:
 * - Navigation functions (next, prev, current, peek)
 * - Swap request preparation
 * - NAE control functions
 * - Timer/dwell accessors
 * - Touch event handlers
 */

#include "play_scheduler.h"
#include "play_scheduler_internal.h"
#include "animation_swap_request.h"
#include "config_store.h"
#include "p3a_state.h"
#include "content_cache.h"
#include "esp_log.h"
#include <string.h>
#include <sys/stat.h>
#include <utime.h>
#include <time.h>

static const char *TAG = "ps_navigation";

// Forward declarations for animation_player functions (implemented in main)
// Using weak symbols to avoid hard dependency on main component
extern esp_err_t animation_player_request_swap(const swap_request_t *request) __attribute__((weak));
extern void animation_player_display_message(const char *title, const char *body) __attribute__((weak));

// ============================================================================
// Swap Request
// ============================================================================

static esp_err_t prepare_and_request_swap(ps_state_t *state, const ps_artwork_t *artwork)
{
    if (!artwork || !ps_file_exists(artwork->filepath)) {
        return ESP_ERR_NOT_FOUND;
    }

    swap_request_t request = {0};
    strlcpy(request.filepath, artwork->filepath, sizeof(request.filepath));
    request.type = artwork->type;
    request.post_id = artwork->post_id;

    // Dwell time: user override > artwork dwell
    if (state->dwell_time_seconds > 0) {
        request.dwell_time_ms = state->dwell_time_seconds * 1000;
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
// Navigation
// ============================================================================

esp_err_t play_scheduler_next(ps_artwork_t *out_artwork)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);

    esp_err_t result = ESP_OK;
    ps_artwork_t artwork;
    bool found = false;

    // If walking forward through history, return from history
    if (ps_history_can_go_forward(s_state)) {
        found = ps_history_go_forward(s_state, &artwork);
    }

    if (!found) {
        // Compute fresh: pick next available artwork using availability masking
        // This iterates through channel entries, skipping files that don't exist
        found = ps_pick_next_available(s_state, &artwork);
        if (found) {
            ps_history_push(s_state, &artwork);
            s_state->last_played_id = artwork.artwork_id;
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
            for (size_t i = 0; i < s_state->channel_count; i++) {
                if (s_state->channels[i].refresh_async_pending || s_state->channels[i].refresh_in_progress) {
                    any_refreshing = true;
                    break;
                }
            }

            extern void p3a_render_set_channel_message(const char *channel_name, int msg_type,
                                                       int progress_percent, const char *detail);

            // Get display name for first channel
            char display_name[64] = "Channel";
            if (s_state->channel_count > 0) {
                ps_get_display_name(s_state->channels[0].channel_id, display_name, sizeof(display_name));
            }

            // Only show loading/downloading messages if we have WiFi connectivity
            if (p3a_state_has_wifi()) {
                if (any_refreshing) {
                    // Channel is still refreshing - show loading message
                    p3a_render_set_channel_message(display_name, 1 /* P3A_CHANNEL_MSG_LOADING */, -1,
                                                    "Updating channel index...");
                } else {
                    // Check if download manager is actively downloading
                    if (content_cache_is_busy()) {
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
        result = prepare_and_request_swap(s_state, &artwork);

        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Swap request failed: %s", esp_err_to_name(result));
        }

        if (out_artwork) {
            memcpy(out_artwork, &artwork, sizeof(ps_artwork_t));
        }
    }

    xSemaphoreGive(s_state->mutex);

    return result;
}

esp_err_t play_scheduler_prev(ps_artwork_t *out_artwork)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);

    esp_err_t result = ESP_OK;
    ps_artwork_t artwork;

    if (!ps_history_can_go_back(s_state)) {
        ESP_LOGD(TAG, "Cannot go back - at history start");
        result = ESP_ERR_NOT_FOUND;
    } else if (!ps_history_go_back(s_state, &artwork)) {
        result = ESP_ERR_NOT_FOUND;
    } else {
        // Request swap
        result = prepare_and_request_swap(s_state, &artwork);

        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Swap request failed: %s", esp_err_to_name(result));
        }

        if (out_artwork) {
            memcpy(out_artwork, &artwork, sizeof(ps_artwork_t));
        }
    }

    xSemaphoreGive(s_state->mutex);

    return result;
}

esp_err_t play_scheduler_peek_next(ps_artwork_t *out_artwork)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized || !out_artwork) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);

    // Peek at what ps_pick_next_available would return without modifying state
    bool found = ps_peek_next_available(s_state, out_artwork);

    xSemaphoreGive(s_state->mutex);

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t play_scheduler_current(ps_artwork_t *out_artwork)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);

    bool found = ps_history_get_current(s_state, out_artwork);

    xSemaphoreGive(s_state->mutex);

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// ============================================================================
// NAE Control
// ============================================================================

void play_scheduler_set_nae_enabled(bool enable)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized) return;
    xSemaphoreTake(s_state->mutex, portMAX_DELAY);
    s_state->nae_enabled = enable;
    xSemaphoreGive(s_state->mutex);
}

bool play_scheduler_is_nae_enabled(void)
{
    ps_state_t *s_state = ps_get_state();
    return s_state->nae_enabled;
}

void play_scheduler_nae_insert(const ps_artwork_t *artwork)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized || !artwork) return;

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);
    ps_nae_insert(s_state, artwork);
    xSemaphoreGive(s_state->mutex);
}

// ============================================================================
// Timer & Dwell
// ============================================================================

void play_scheduler_set_dwell_time(uint32_t seconds)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized) return;
    xSemaphoreTake(s_state->mutex, portMAX_DELAY);
    s_state->dwell_time_seconds = seconds;
    ps_timer_reset(s_state);
    xSemaphoreGive(s_state->mutex);
    ESP_LOGI(TAG, "Dwell time set to %lu seconds", (unsigned long)seconds);
}

uint32_t play_scheduler_get_dwell_time(void)
{
    ps_state_t *s_state = ps_get_state();
    return s_state->dwell_time_seconds;
}

void play_scheduler_reset_timer(void)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized) return;
    ps_timer_reset(s_state);
}

// ============================================================================
// Touch Events
// ============================================================================

void play_scheduler_touch_next(void)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized) return;
    play_scheduler_next(NULL);
}

void play_scheduler_touch_back(void)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized) return;
    play_scheduler_prev(NULL);
}
