// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file p3a_state.c
 * @brief Core state machine: init/deinit, queries, transitions, sub-states,
 *        app status, callbacks, playset NVS persistence
 */

#include "p3a_state_internal.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "p3a_state";

// NVS storage
#define NVS_NAMESPACE "p3a_state"
#define NVS_KEY_LAST_STATE "last_state"
#define NVS_KEY_ACTIVE_PLAYSET "playset"    // Active playset name (e.g., "channel_recent")

// Global shared state (non-static so other p3a_state_*.c files can access via internal header)
p3a_state_internal_t s_state = {0};

// ============================================================================
// Helper Functions
// ============================================================================

void p3a_state_notify_callbacks(p3a_state_t old_state, p3a_state_t new_state)
{
    for (int i = 0; i < s_state.callback_count; i++) {
        if (s_state.callbacks[i].callback) {
            s_state.callbacks[i].callback(old_state, new_state, s_state.callbacks[i].user_data);
        }
    }
}

void p3a_state_update_channel_display_name(p3a_channel_info_t *info)
{
    switch (info->type) {
        case P3A_CHANNEL_SDCARD:
            snprintf(info->display_name, sizeof(info->display_name), "SD Card");
            break;
        case P3A_CHANNEL_MAKAPIX_ALL:
            snprintf(info->display_name, sizeof(info->display_name), "Makapix: All");
            break;
        case P3A_CHANNEL_MAKAPIX_PROMOTED:
            snprintf(info->display_name, sizeof(info->display_name), "Makapix: Featured");
            break;
        case P3A_CHANNEL_MAKAPIX_USER:
            snprintf(info->display_name, sizeof(info->display_name), "Makapix: Following");
            break;
        case P3A_CHANNEL_MAKAPIX_BY_USER:
            // "Makapix: @" is 11 bytes, so we have 64 - 11 = 53 bytes for identifier
            snprintf(info->display_name, sizeof(info->display_name), "Makapix: @%.53s", info->identifier);
            break;
        case P3A_CHANNEL_MAKAPIX_HASHTAG:
            // "Makapix: #" is 11 bytes, so we have 64 - 11 = 53 bytes for identifier
            snprintf(info->display_name, sizeof(info->display_name), "Makapix: #%.53s", info->identifier);
            break;
        case P3A_CHANNEL_MAKAPIX_ARTWORK:
            snprintf(info->display_name, sizeof(info->display_name), "Single Artwork");
            break;
        case P3A_CHANNEL_GIPHY_TRENDING:
            snprintf(info->display_name, sizeof(info->display_name), "Giphy: Trending");
            break;
        default:
            snprintf(info->display_name, sizeof(info->display_name), "Unknown");
            break;
    }
}

// ============================================================================
// State Entry Rules
// ============================================================================

/**
 * @brief Check if transition to target state is allowed from current state
 */
static bool can_enter_state(p3a_state_t target)
{
    p3a_state_t current = s_state.current_state;

    switch (target) {
        case P3A_STATE_ANIMATION_PLAYBACK:
            // Can always enter animation playback
            return true;

        case P3A_STATE_PROVISIONING:
            // Can only enter from animation playback
            // Cannot enter during OTA
            return (current == P3A_STATE_ANIMATION_PLAYBACK);

        case P3A_STATE_OTA:
            // Can only enter from animation playback
            // Cannot enter during provisioning or PICO-8
            return (current == P3A_STATE_ANIMATION_PLAYBACK);

        case P3A_STATE_PICO8_STREAMING:
            // Can only enter from animation playback
            return (current == P3A_STATE_ANIMATION_PLAYBACK);

        default:
            return false;
    }
}

// ============================================================================
// Persistence
// ============================================================================

esp_err_t p3a_state_set_active_playset(const char *name)
{
    if (!s_state.initialized || !s_state.mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!name) {
        name = "";  // Allow clearing the playset
    }

    // Validate name length
    size_t name_len = strlen(name);
    if (name_len > P3A_PLAYSET_MAX_NAME_LEN) {
        ESP_LOGW(TAG, "Playset name too long: %zu (max %d)", name_len, P3A_PLAYSET_MAX_NAME_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    // Update in-memory state
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    strlcpy(s_state.active_playset, name, sizeof(s_state.active_playset));
    xSemaphoreGive(s_state.mutex);

    // Persist to NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for writing playset: %s", esp_err_to_name(err));
        return err;
    }

    if (name_len > 0) {
        err = nvs_set_str(handle, NVS_KEY_ACTIVE_PLAYSET, name);
    } else {
        // Clear the key if name is empty
        err = nvs_erase_key(handle, NVS_KEY_ACTIVE_PLAYSET);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;  // Key didn't exist, that's fine
        }
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Persisted active playset: '%s'", name);
    } else {
        ESP_LOGW(TAG, "Failed to persist playset: %s", esp_err_to_name(err));
    }

    return err;
}

const char *p3a_state_get_active_playset(void)
{
    if (!s_state.initialized) {
        return "";
    }

    // Return pointer to internal buffer (thread-safe for reading)
    return s_state.active_playset;
}

// ============================================================================
// Initialization
// ============================================================================

esp_err_t p3a_state_init(void)
{
    if (s_state.initialized) {
        ESP_LOGW(TAG, "State machine already initialized");
        return ESP_OK;
    }

    // Create mutex
    s_state.mutex = xSemaphoreCreateMutex();
    if (!s_state.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Initialize connectivity tracking
    esp_err_t conn_err = p3a_state_connectivity_init();
    if (conn_err != ESP_OK) {
        ESP_LOGW(TAG, "Connectivity init failed: %s", esp_err_to_name(conn_err));
    }

    // Load persisted playset from NVS
    memset(s_state.active_playset, 0, sizeof(s_state.active_playset));
    nvs_handle_t handle;
    esp_err_t nvs_err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (nvs_err == ESP_OK) {
        size_t len = sizeof(s_state.active_playset);
        nvs_err = nvs_get_str(handle, NVS_KEY_ACTIVE_PLAYSET, s_state.active_playset, &len);
        if (nvs_err != ESP_OK && nvs_err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to load playset from NVS: %s", esp_err_to_name(nvs_err));
        }
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Loaded active playset: '%s'", s_state.active_playset);

    // Default channel until playset system takes over
    s_state.current_channel.type = P3A_CHANNEL_SDCARD;
    p3a_state_update_channel_display_name(&s_state.current_channel);

    // Initialize to animation playback state with "Starting..." message
    // This prevents blank screen gap between boot logo and first content
    s_state.current_state = P3A_STATE_ANIMATION_PLAYBACK;
    s_state.previous_state = P3A_STATE_ANIMATION_PLAYBACK;
    s_state.playback_substate = P3A_PLAYBACK_CHANNEL_MESSAGE;
    s_state.channel_message.type = P3A_CHANNEL_MSG_LOADING;
    snprintf(s_state.channel_message.channel_name,
             sizeof(s_state.channel_message.channel_name), "p3a");
    snprintf(s_state.channel_message.detail,
             sizeof(s_state.channel_message.detail), "Starting...");
    s_state.channel_message.progress_percent = -1;
    s_state.app_status = P3A_APP_STATUS_READY;
    s_state.callback_count = 0;

    s_state.initialized = true;

    ESP_LOGI(TAG, "State machine initialized, starting in ANIMATION_PLAYBACK with channel: %s",
             s_state.current_channel.display_name);

    return ESP_OK;
}

void p3a_state_deinit(void)
{
    if (!s_state.initialized) return;

    p3a_state_connectivity_deinit();

    if (s_state.mutex) {
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
    }

    s_state.initialized = false;
    ESP_LOGI(TAG, "State machine deinitialized");
}

// ============================================================================
// State Queries
// ============================================================================

p3a_state_t p3a_state_get(void)
{
    if (!s_state.initialized) return P3A_STATE_ANIMATION_PLAYBACK;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    p3a_state_t state = s_state.current_state;
    xSemaphoreGive(s_state.mutex);

    return state;
}

const char *p3a_state_get_name(p3a_state_t state)
{
    switch (state) {
        case P3A_STATE_BOOT: return "BOOT";
        case P3A_STATE_ANIMATION_PLAYBACK: return "ANIMATION_PLAYBACK";
        case P3A_STATE_PROVISIONING: return "PROVISIONING";
        case P3A_STATE_OTA: return "OTA";
        case P3A_STATE_PICO8_STREAMING: return "PICO8_STREAMING";
        case P3A_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

p3a_app_status_t p3a_state_get_app_status(void)
{
    if (!s_state.initialized) return P3A_APP_STATUS_READY;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    p3a_app_status_t status = s_state.app_status;
    xSemaphoreGive(s_state.mutex);

    return status;
}

const char *p3a_state_get_app_status_name(p3a_app_status_t status)
{
    switch (status) {
        case P3A_APP_STATUS_READY: return "READY";
        case P3A_APP_STATUS_PROCESSING: return "PROCESSING";
        case P3A_APP_STATUS_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

p3a_playback_substate_t p3a_state_get_playback_substate(void)
{
    if (!s_state.initialized) return P3A_PLAYBACK_PLAYING;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    p3a_playback_substate_t substate = s_state.playback_substate;
    xSemaphoreGive(s_state.mutex);

    return substate;
}

p3a_provisioning_substate_t p3a_state_get_provisioning_substate(void)
{
    if (!s_state.initialized) return P3A_PROV_STATUS;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    p3a_provisioning_substate_t substate = s_state.provisioning_substate;
    xSemaphoreGive(s_state.mutex);

    return substate;
}

p3a_ota_substate_t p3a_state_get_ota_substate(void)
{
    if (!s_state.initialized) return P3A_OTA_CHECKING;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    p3a_ota_substate_t substate = s_state.ota_substate;
    xSemaphoreGive(s_state.mutex);

    return substate;
}

esp_err_t p3a_state_get_channel_info(p3a_channel_info_t *out_info)
{
    if (!out_info) return ESP_ERR_INVALID_ARG;
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    memcpy(out_info, &s_state.current_channel, sizeof(p3a_channel_info_t));
    xSemaphoreGive(s_state.mutex);

    return ESP_OK;
}

esp_err_t p3a_state_get_channel_message(p3a_channel_message_t *out_msg)
{
    if (!out_msg) return ESP_ERR_INVALID_ARG;
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    memcpy(out_msg, &s_state.channel_message, sizeof(p3a_channel_message_t));
    xSemaphoreGive(s_state.mutex);

    return ESP_OK;
}

// ============================================================================
// State Transitions
// ============================================================================

esp_err_t p3a_state_enter_animation_playback(void)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    if (!can_enter_state(P3A_STATE_ANIMATION_PLAYBACK)) {
        xSemaphoreGive(s_state.mutex);
        ESP_LOGW(TAG, "Cannot enter ANIMATION_PLAYBACK from %s",
                 p3a_state_get_name(s_state.current_state));
        return ESP_ERR_INVALID_STATE;
    }

    p3a_state_t old_state = s_state.current_state;
    s_state.previous_state = old_state;
    s_state.current_state = P3A_STATE_ANIMATION_PLAYBACK;
    s_state.playback_substate = P3A_PLAYBACK_PLAYING;

    xSemaphoreGive(s_state.mutex);

    ESP_LOGI(TAG, "State transition: %s -> ANIMATION_PLAYBACK",
             p3a_state_get_name(old_state));

    p3a_state_notify_callbacks(old_state, P3A_STATE_ANIMATION_PLAYBACK);

    return ESP_OK;
}

esp_err_t p3a_state_enter_provisioning(void)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    if (!can_enter_state(P3A_STATE_PROVISIONING)) {
        xSemaphoreGive(s_state.mutex);
        ESP_LOGW(TAG, "Cannot enter PROVISIONING from %s (entry rule denied)",
                 p3a_state_get_name(s_state.current_state));
        return ESP_ERR_INVALID_STATE;
    }

    p3a_state_t old_state = s_state.current_state;
    s_state.previous_state = old_state;
    s_state.current_state = P3A_STATE_PROVISIONING;
    s_state.provisioning_substate = P3A_PROV_STATUS;
    memset(s_state.provisioning_status, 0, sizeof(s_state.provisioning_status));
    snprintf(s_state.provisioning_status, sizeof(s_state.provisioning_status), "Starting...");

    xSemaphoreGive(s_state.mutex);

    ESP_LOGI(TAG, "State transition: %s -> PROVISIONING",
             p3a_state_get_name(old_state));

    p3a_state_notify_callbacks(old_state, P3A_STATE_PROVISIONING);

    return ESP_OK;
}

esp_err_t p3a_state_enter_ota(void)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    if (!can_enter_state(P3A_STATE_OTA)) {
        xSemaphoreGive(s_state.mutex);
        ESP_LOGW(TAG, "Cannot enter OTA from %s (entry rule denied)",
                 p3a_state_get_name(s_state.current_state));
        return ESP_ERR_INVALID_STATE;
    }

    p3a_state_t old_state = s_state.current_state;
    s_state.previous_state = old_state;
    s_state.current_state = P3A_STATE_OTA;
    s_state.ota_substate = P3A_OTA_CHECKING;
    s_state.ota_progress_percent = 0;
    memset(s_state.ota_status_text, 0, sizeof(s_state.ota_status_text));

    xSemaphoreGive(s_state.mutex);

    ESP_LOGI(TAG, "State transition: %s -> OTA",
             p3a_state_get_name(old_state));

    p3a_state_notify_callbacks(old_state, P3A_STATE_OTA);

    return ESP_OK;
}

esp_err_t p3a_state_enter_pico8_streaming(void)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    if (!can_enter_state(P3A_STATE_PICO8_STREAMING)) {
        xSemaphoreGive(s_state.mutex);
        ESP_LOGW(TAG, "Cannot enter PICO8_STREAMING from %s (entry rule denied)",
                 p3a_state_get_name(s_state.current_state));
        return ESP_ERR_INVALID_STATE;
    }

    p3a_state_t old_state = s_state.current_state;
    s_state.previous_state = old_state;
    s_state.current_state = P3A_STATE_PICO8_STREAMING;

    xSemaphoreGive(s_state.mutex);

    ESP_LOGI(TAG, "State transition: %s -> PICO8_STREAMING",
             p3a_state_get_name(old_state));

    p3a_state_notify_callbacks(old_state, P3A_STATE_PICO8_STREAMING);

    return ESP_OK;
}

esp_err_t p3a_state_exit_to_playback(void)
{
    return p3a_state_enter_animation_playback();
}

esp_err_t p3a_state_enter_error(void)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    p3a_state_t old_state = s_state.current_state;
    s_state.previous_state = old_state;
    s_state.current_state = P3A_STATE_ERROR;

    xSemaphoreGive(s_state.mutex);

    ESP_LOGI(TAG, "State transition: %s -> ERROR", p3a_state_get_name(old_state));
    p3a_state_notify_callbacks(old_state, P3A_STATE_ERROR);

    return ESP_OK;
}

// ============================================================================
// Sub-state Updates
// ============================================================================

void p3a_state_set_playback_playing(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.playback_substate = P3A_PLAYBACK_PLAYING;
    s_state.channel_message.type = P3A_CHANNEL_MSG_NONE;
    xSemaphoreGive(s_state.mutex);
}

void p3a_state_set_channel_message(const p3a_channel_message_t *msg)
{
    if (!s_state.initialized || !msg) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    // Channel message substates only apply during animation playback.
    // Reject when in provisioning, OTA, or other exclusive states to
    // prevent background downloads from corrupting the active UI.
    if (s_state.current_state != P3A_STATE_ANIMATION_PLAYBACK) {
        xSemaphoreGive(s_state.mutex);
        return;
    }
    // IMPORTANT: Treat "NONE" as clearing the channel message and returning to normal playback.
    // Otherwise the renderer stays in CHANNEL_MESSAGE mode forever, which prevents normal
    // animation playback (and swap/prefetch processing) from running.
    if (msg->type == P3A_CHANNEL_MSG_NONE) {
        s_state.playback_substate = P3A_PLAYBACK_PLAYING;
        memset(&s_state.channel_message, 0, sizeof(s_state.channel_message));
        s_state.channel_message.type = P3A_CHANNEL_MSG_NONE;
    } else {
        s_state.playback_substate = P3A_PLAYBACK_CHANNEL_MESSAGE;
        memcpy(&s_state.channel_message, msg, sizeof(p3a_channel_message_t));
    }
    xSemaphoreGive(s_state.mutex);
}

void p3a_state_set_provisioning_substate(p3a_provisioning_substate_t substate)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.provisioning_substate = substate;
    xSemaphoreGive(s_state.mutex);

    ESP_LOGD(TAG, "Provisioning sub-state: %d", substate);
}

void p3a_state_set_ota_substate(p3a_ota_substate_t substate)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.ota_substate = substate;
    xSemaphoreGive(s_state.mutex);

    ESP_LOGD(TAG, "OTA sub-state: %d", substate);
}

void p3a_state_set_ota_progress(int percent, const char *status_text)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.ota_progress_percent = percent;
    if (status_text) {
        snprintf(s_state.ota_status_text, sizeof(s_state.ota_status_text), "%s", status_text);
    }
    xSemaphoreGive(s_state.mutex);
}

// ============================================================================
// App Status
// ============================================================================

void p3a_state_set_app_status(p3a_app_status_t status)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    p3a_app_status_t old_status = s_state.app_status;
    s_state.app_status = status;
    xSemaphoreGive(s_state.mutex);

    if (old_status != status) {
        ESP_LOGI(TAG, "App status: %s -> %s",
                 p3a_state_get_app_status_name(old_status),
                 p3a_state_get_app_status_name(status));
    }
}

void p3a_state_enter_ready(void)
{
    p3a_state_set_app_status(P3A_APP_STATUS_READY);
}

void p3a_state_enter_processing(void)
{
    p3a_state_set_app_status(P3A_APP_STATUS_PROCESSING);
}

void p3a_state_enter_app_error(void)
{
    p3a_state_set_app_status(P3A_APP_STATUS_ERROR);
}

// ============================================================================
// Callbacks
// ============================================================================

esp_err_t p3a_state_register_callback(p3a_state_change_cb_t callback, void *user_data)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    if (s_state.callback_count >= MAX_CALLBACKS) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_NO_MEM;
    }

    s_state.callbacks[s_state.callback_count].callback = callback;
    s_state.callbacks[s_state.callback_count].user_data = user_data;
    s_state.callback_count++;

    xSemaphoreGive(s_state.mutex);

    return ESP_OK;
}

void p3a_state_unregister_callback(p3a_state_change_cb_t callback)
{
    if (!callback || !s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    for (int i = 0; i < s_state.callback_count; i++) {
        if (s_state.callbacks[i].callback == callback) {
            // Shift remaining callbacks
            for (int j = i; j < s_state.callback_count - 1; j++) {
                s_state.callbacks[j] = s_state.callbacks[j + 1];
            }
            s_state.callback_count--;
            break;
        }
    }

    xSemaphoreGive(s_state.mutex);
}
