/**
 * @file p3a_render.c
 * @brief State-aware rendering dispatch implementation
 */

#include "p3a_render.h"
#include "p3a_state.h"
#include "p3a_boot_logo.h"
#include "p3a_board.h"
#include "esp_log.h"
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
            p3a_playback_substate_t substate = p3a_state_get_playback_substate();
            if (substate == P3A_PLAYBACK_CHANNEL_MESSAGE) {
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
        result->frame_delay_ms = delay;
        result->buffer_modified = true;
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

void p3a_render_set_channel_message(const char *channel_name,
                                     int msg_type,
                                     int progress_percent,
                                     const char *detail)
{
    if (!s_render.initialized) return;
    
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
    
    ESP_LOGD(TAG, "Channel message: %s - %s (%d%%)",
             channel_name ? channel_name : "",
             channel_msg_type_to_string(s_render.channel_msg_type),
             progress_percent);
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

