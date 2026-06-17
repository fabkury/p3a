// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file p3a_render.c
 * @brief State-aware rendering dispatch implementation
 */

#include "p3a_render.h"
#include "p3a_state.h"
#include "p3a_boot_logo.h"
#include "p3a_board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "p3a_render";

// ============================================================================
// Render state (cached for UI rendering)
// ============================================================================

static struct {
    // Channel message state
    char channel_name[64];
    p3a_channel_msg_type_t channel_msg_type;
    int channel_progress_percent;
    char channel_detail[128];
    // Message arbitration (see p3a_render_set_channel_message)
    int64_t channel_msg_changed_us;   // last accepted identity change
    int64_t channel_msg_updated_us;   // last accepted write (incl. progress)
    int64_t channel_msg_expire_us;    // absolute time to auto-clear; 0 = persist

    // Provisioning state
    char prov_status[128];
    char prov_code[16];
    time_t prov_expires_time;
    
    // OTA state
    int ota_progress;
    char ota_status[64];
    char ota_version_from[32];
    char ota_version_to[32];
    
    bool initialized;
} s_render = {0};

// ============================================================================
// External render functions (implemented in ugfx_ui.c or animation_player.c)
// ============================================================================

// µGFX UI rendering functions (weak symbols - implemented elsewhere)
extern int ugfx_ui_render_to_buffer(uint8_t *buffer, size_t stride) __attribute__((weak));
extern esp_err_t ugfx_ui_init(void) __attribute__((weak));
extern esp_err_t ugfx_ui_show_channel_message(const char *channel_name, const char *message, int progress_percent) __attribute__((weak));
extern void ugfx_ui_hide_channel_message(void) __attribute__((weak));
extern esp_err_t ugfx_ui_show_slave_ota_progress(const char *version_from, const char *version_to) __attribute__((weak));
extern void ugfx_ui_update_slave_ota_progress(int percent, const char *status_text) __attribute__((weak));
extern void ugfx_ui_hide_slave_ota_progress(void) __attribute__((weak));

// Animation rendering (weak symbol)
extern int animation_player_render_frame_internal(uint8_t *buffer, size_t stride) __attribute__((weak));

// ============================================================================
// Forward declarations
// ============================================================================

static esp_err_t render_animation_playback(uint8_t *buffer, size_t stride, p3a_render_result_t *result);
static esp_err_t render_channel_message(uint8_t *buffer, size_t stride, p3a_render_result_t *result);
static esp_err_t render_provisioning(uint8_t *buffer, size_t stride, p3a_render_result_t *result);
static esp_err_t render_ota(uint8_t *buffer, size_t stride, p3a_render_result_t *result);

// ============================================================================
// Helper functions
// ============================================================================

static const char *channel_msg_type_to_string(p3a_channel_msg_type_t type)
{
    switch (type) {
        case P3A_CHANNEL_MSG_NONE: return "";
        case P3A_CHANNEL_MSG_FETCHING: return "Fetching artwork";
        case P3A_CHANNEL_MSG_DOWNLOADING: return "Downloading artwork";
        case P3A_CHANNEL_MSG_DOWNLOAD_FAILED: return "Download failed, retrying";
        case P3A_CHANNEL_MSG_EMPTY: return "Channel empty";
        case P3A_CHANNEL_MSG_LOADING: return "Loading channel";
        case P3A_CHANNEL_MSG_ERROR: return "Failed to load channel";
        default: return "Unknown";
    }
}

// Arbitration tuning for the channel-message slot. A *different* message may
// replace the current one only after the current one has been visible for
// MIN_DISPLAY (equal rank), immediately (higher rank), or once the current
// one has stopped receiving updates for STALE (lower rank — a dead progress
// bar must not wedge the screen).
#define CHANNEL_MSG_MIN_DISPLAY_US  (700LL * 1000)
#define CHANNEL_MSG_STALE_US        (2000LL * 1000)

static int channel_msg_priority(p3a_channel_msg_type_t type)
{
    switch (type) {
        case P3A_CHANNEL_MSG_ERROR:
        case P3A_CHANNEL_MSG_DOWNLOAD_FAILED:
        case P3A_CHANNEL_MSG_EMPTY:
            return 2;   // problem/terminal states must not be lost
        case P3A_CHANNEL_MSG_DOWNLOADING:
            return 1;   // live download progress beats generic loading
        default:
            return 0;   // LOADING / FETCHING
    }
}

// ============================================================================
// Initialization
// ============================================================================

esp_err_t p3a_render_init(void)
{
    if (s_render.initialized) {
        return ESP_OK;
    }
    
    memset(&s_render, 0, sizeof(s_render));
    s_render.initialized = true;
    
    ESP_LOGI(TAG, "Render system initialized");
    return ESP_OK;
}

// ============================================================================
// Main render dispatch
// ============================================================================

esp_err_t p3a_render_frame(uint8_t *buffer, size_t stride, p3a_render_result_t *result)
{
    if (!buffer || !result) return ESP_ERR_INVALID_ARG;
    if (!s_render.initialized) return ESP_ERR_INVALID_STATE;
    
    result->frame_delay_ms = 100;  // Default
    result->buffer_modified = false;
    
    // Boot logo has highest priority - exclusive screen access during boot
    if (p3a_boot_logo_is_active()) {
        int delay = p3a_boot_logo_render(buffer, P3A_DISPLAY_WIDTH, P3A_DISPLAY_HEIGHT, stride);
        if (delay > 0) {
            result->frame_delay_ms = delay;
            result->buffer_modified = true;
            return ESP_OK;
        }
        // If delay < 0, boot logo just expired, fall through to normal rendering
    }
    
    p3a_state_t state = p3a_state_get();
    
    switch (state) {
        case P3A_STATE_ANIMATION_PLAYBACK: {
            // Auto-dismiss an expired channel message (e.g. the 429 rate-limit
            // notice's 20s cap) before deciding what to render, so this very
            // frame reverts to animation playback instead of waiting for the
            // next swap to clear the overlay.
            if (s_render.channel_msg_expire_us != 0 &&
                esp_timer_get_time() >= s_render.channel_msg_expire_us) {
                p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
            }
            p3a_playback_substate_t substate = p3a_state_get_playback_substate();
            if (substate == P3A_PLAYBACK_CHANNEL_MESSAGE) {
                // IMPORTANT: Still call animation renderer to process prefetch/swap
                // even when showing a channel message. This prevents the loader task
                // from getting stuck waiting for prefetch to complete.
                if (animation_player_render_frame_internal) {
                    (void)animation_player_render_frame_internal(buffer, stride);
                }
                return render_channel_message(buffer, stride, result);
            } else {
                return render_animation_playback(buffer, stride, result);
            }
        }
        
        case P3A_STATE_PROVISIONING:
            return render_provisioning(buffer, stride, result);
            
        case P3A_STATE_OTA:
            return render_ota(buffer, stride, result);
            
        case P3A_STATE_PICO8_STREAMING:
            // PICO-8 renders externally - we don't render anything
            result->buffer_modified = false;
            result->frame_delay_ms = -1;
            return ESP_OK;
            
        default:
            ESP_LOGW(TAG, "Unknown state %d", state);
            return ESP_ERR_INVALID_STATE;
    }
}

bool p3a_render_needs_frame(void)
{
    p3a_state_t state = p3a_state_get();
    
    // PICO-8 doesn't need us to render frames - they come externally
    return (state != P3A_STATE_PICO8_STREAMING);
}

// ============================================================================
// State-specific renderers
// ============================================================================

static esp_err_t render_animation_playback(uint8_t *buffer, size_t stride, p3a_render_result_t *result)
{
    // Delegate to animation player's internal render function
    if (animation_player_render_frame_internal) {
        int delay = animation_player_render_frame_internal(buffer, stride);
        if (delay >= 0) {
            result->frame_delay_ms = delay;
            result->buffer_modified = true;
            return ESP_OK;
        }
        
        // Animation renderer returned -1 (no animation loaded)
        // Fall back to channel message UI if available
        if (ugfx_ui_render_to_buffer) {
            delay = ugfx_ui_render_to_buffer(buffer, stride);
            result->frame_delay_ms = delay > 0 ? delay : 100;
            result->buffer_modified = (delay >= 0);
            return ESP_OK;
        }
        
        // No UI available either - signal buffer not modified
        result->frame_delay_ms = 100;
        result->buffer_modified = false;
        return ESP_OK;
    }
    
    // Fallback - shouldn't happen in production
    ESP_LOGW(TAG, "No animation renderer available");
    result->frame_delay_ms = 100;
    result->buffer_modified = false;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t render_channel_message(uint8_t *buffer, size_t stride, p3a_render_result_t *result)
{
    // Render channel message using µGFX
    // For now, delegate to ugfx_ui which already has text rendering capability
    
    if (ugfx_ui_render_to_buffer) {
        // The ugfx_ui module will check internal state and render appropriate screen
        int delay = ugfx_ui_render_to_buffer(buffer, stride);
        result->frame_delay_ms = delay > 0 ? delay : 100;
        result->buffer_modified = true;
        return ESP_OK;
    }
    
    ESP_LOGW(TAG, "No UI renderer available for channel message");
    result->frame_delay_ms = 100;
    result->buffer_modified = false;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t render_provisioning(uint8_t *buffer, size_t stride, p3a_render_result_t *result)
{
    // Delegate to µGFX UI for provisioning screens
    if (ugfx_ui_render_to_buffer) {
        int delay = ugfx_ui_render_to_buffer(buffer, stride);
        result->frame_delay_ms = delay > 0 ? delay : 100;
        result->buffer_modified = true;
        return ESP_OK;
    }
    
    ESP_LOGW(TAG, "No UI renderer available for provisioning");
    result->frame_delay_ms = 100;
    result->buffer_modified = false;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t render_ota(uint8_t *buffer, size_t stride, p3a_render_result_t *result)
{
    // Delegate to µGFX UI for OTA progress screen
    if (ugfx_ui_render_to_buffer) {
        int delay = ugfx_ui_render_to_buffer(buffer, stride);
        result->frame_delay_ms = delay > 0 ? delay : 100;
        result->buffer_modified = true;
        return ESP_OK;
    }
    
    ESP_LOGW(TAG, "No UI renderer available for OTA");
    result->frame_delay_ms = 100;
    result->buffer_modified = false;
    return ESP_ERR_NOT_SUPPORTED;
}

// ============================================================================
// State update functions (called by other modules to set render data)
// ============================================================================

static void set_channel_message_impl(const char *channel_name,
                                     int msg_type,
                                     int progress_percent,
                                     const char *detail,
                                     uint32_t ttl_ms)
{
    if (!s_render.initialized) return;

    // Channel messages only apply during animation playback.
    // Reject when in provisioning, OTA, or other exclusive UI states
    // to prevent download progress from overwriting the active UI screen.
    p3a_state_t state = p3a_state_get();
    if (state != P3A_STATE_ANIMATION_PLAYBACK) {
        ESP_LOGD(TAG, "Ignoring channel message in state %s",
                 p3a_state_get_name(state));
        return;
    }

    // Arbitration: this slot used to be last-writer-wins, and during a cold
    // playset switch four uncoordinated writers (executor, refresh starts,
    // download starts/progress, failed picks) made the screen flicker
    // erratically. Rules:
    //   - clears (MSG_NONE) always pass — blocking one would leave a stale
    //     overlay covering a freshly swapped-in artwork;
    //   - updates to the SAME message (same type + channel) always pass, so
    //     progress bars stay live;
    //   - a DIFFERENT message passes if it outranks the current one, or
    //     after the current one has been shown CHANNEL_MSG_MIN_DISPLAY_US
    //     (equal rank), or once the current one has gone stale (lower rank).
    // A NULL channel_name inherits the currently displayed name (existing
    // semantics of the snprintf guard below), so it compares as same-name.
    {
        const char *eff_name = channel_name ? channel_name : s_render.channel_name;
        bool same_identity =
            ((p3a_channel_msg_type_t)msg_type == s_render.channel_msg_type) &&
            (strcmp(eff_name, s_render.channel_name) == 0);
        int64_t now_us = esp_timer_get_time();

        if (msg_type != P3A_CHANNEL_MSG_NONE) {
            if (!same_identity && s_render.channel_msg_type != P3A_CHANNEL_MSG_NONE) {
                int new_prio = channel_msg_priority((p3a_channel_msg_type_t)msg_type);
                int cur_prio = channel_msg_priority(s_render.channel_msg_type);
                bool accept;
                if (new_prio > cur_prio) {
                    accept = true;
                } else if (new_prio == cur_prio) {
                    accept = (now_us - s_render.channel_msg_changed_us) >= CHANNEL_MSG_MIN_DISPLAY_US;
                } else {
                    accept = (now_us - s_render.channel_msg_updated_us) >= CHANNEL_MSG_STALE_US;
                }
                if (!accept) {
                    ESP_LOGD(TAG, "Channel message dropped (arbitration): %s - %s",
                             eff_name,
                             channel_msg_type_to_string((p3a_channel_msg_type_t)msg_type));
                    return;
                }
            }
            if (!same_identity) {
                s_render.channel_msg_changed_us = now_us;
            }
            s_render.channel_msg_updated_us = now_us;
        }
    }

    if (channel_name) {
        snprintf(s_render.channel_name, sizeof(s_render.channel_name), "%s", channel_name);
    }
    s_render.channel_msg_type = (p3a_channel_msg_type_t)msg_type;
    s_render.channel_progress_percent = progress_percent;
    if (detail) {
        snprintf(s_render.channel_detail, sizeof(s_render.channel_detail), "%s", detail);
    } else {
        s_render.channel_detail[0] = '\0';
    }

    // Arm/cancel the one-shot auto-dismiss. Only a non-clear message with a
    // positive TTL expires; everything else (a clear, or a TTL-less message
    // that replaces an expiring one) cancels any pending expiry so it can't
    // dismiss the wrong message later.
    if (msg_type != P3A_CHANNEL_MSG_NONE && ttl_ms > 0) {
        s_render.channel_msg_expire_us = esp_timer_get_time() + (int64_t)ttl_ms * 1000;
    } else {
        s_render.channel_msg_expire_us = 0;
    }
    
    // Also update the state machine's channel message
    p3a_channel_message_t msg = {
        .type = (p3a_channel_msg_type_t)msg_type,
        .progress_percent = progress_percent
    };
    if (channel_name) {
        snprintf(msg.channel_name, sizeof(msg.channel_name), "%s", channel_name);
    }
    if (detail) {
        snprintf(msg.detail, sizeof(msg.detail), "%s", detail);
    }
    p3a_state_set_channel_message(&msg);
    
    // Activate/deactivate µGFX channel message UI
    // This is required for the UI to actually render the message
    if (msg_type != P3A_CHANNEL_MSG_NONE) {
        if (ugfx_ui_show_channel_message) {
            ugfx_ui_show_channel_message(channel_name, detail, progress_percent);
        }
    } else {
        if (ugfx_ui_hide_channel_message) {
            ugfx_ui_hide_channel_message();
        }
    }
    
    ESP_LOGD(TAG, "Channel message: %s - %s (%d%%)",
             channel_name ? channel_name : "",
             channel_msg_type_to_string(s_render.channel_msg_type),
             progress_percent);
}

void p3a_render_set_channel_message(const char *channel_name,
                                     int msg_type,
                                     int progress_percent,
                                     const char *detail)
{
    set_channel_message_impl(channel_name, msg_type, progress_percent, detail, 0);
}

void p3a_render_set_channel_message_ttl(const char *channel_name,
                                         int msg_type,
                                         int progress_percent,
                                         const char *detail,
                                         uint32_t ttl_ms)
{
    set_channel_message_impl(channel_name, msg_type, progress_percent, detail, ttl_ms);
}

void p3a_render_set_provisioning_status(const char *status)
{
    if (!s_render.initialized || !status) return;
    
    snprintf(s_render.prov_status, sizeof(s_render.prov_status), "%s", status);
    
    ESP_LOGD(TAG, "Provisioning status: %s", status);
}

void p3a_render_set_provisioning_code(const char *code, const char *expires_at)
{
    if (!s_render.initialized) return;
    
    if (code) {
        snprintf(s_render.prov_code, sizeof(s_render.prov_code), "%s", code);
    }
    
    if (expires_at) {
        // Parse ISO 8601 timestamp to time_t
        struct tm tm = {0};
        // Format: "2025-01-15T12:30:00Z"
        if (sscanf(expires_at, "%d-%d-%dT%d:%d:%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            s_render.prov_expires_time = mktime(&tm);
        }
    }
    
    ESP_LOGD(TAG, "Provisioning code: %s", code ? code : "(null)");
}

void p3a_render_set_ota_progress(int percent, const char *status,
                                  const char *version_from, const char *version_to)
{
    if (!s_render.initialized) return;

    s_render.ota_progress = percent;

    if (status) {
        snprintf(s_render.ota_status, sizeof(s_render.ota_status), "%s", status);
    }
    if (version_from) {
        snprintf(s_render.ota_version_from, sizeof(s_render.ota_version_from), "%s", version_from);
    }
    if (version_to) {
        snprintf(s_render.ota_version_to, sizeof(s_render.ota_version_to), "%s", version_to);
    }

    ESP_LOGD(TAG, "OTA progress: %d%% - %s", percent, status ? status : "");
}

void p3a_render_show_slave_ota(const char *version_from, const char *version_to)
{
    if (ugfx_ui_show_slave_ota_progress) {
        ugfx_ui_show_slave_ota_progress(version_from, version_to);
    }
}

void p3a_render_update_slave_ota(int percent, const char *status)
{
    if (ugfx_ui_update_slave_ota_progress) {
        ugfx_ui_update_slave_ota_progress(percent, status);
    }
}

void p3a_render_hide_slave_ota(void)
{
    if (ugfx_ui_hide_slave_ota_progress) {
        ugfx_ui_hide_slave_ota_progress();
    }
}

