/**
 * @file p3a_state.c
 * @brief Unified p3a State Machine Implementation
 */

#include "p3a_state.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "p3a_state";

// NVS storage
#define NVS_NAMESPACE "p3a_state"
#define NVS_KEY_CHANNEL_TYPE "ch_type"
#define NVS_KEY_CHANNEL_IDENT "ch_ident"
#define NVS_KEY_LAST_STATE "last_state"

// Maximum callbacks
#define MAX_CALLBACKS 8

// ============================================================================
// Internal State
// ============================================================================

typedef struct {
    // Global state
    p3a_state_t current_state;
    p3a_state_t previous_state;
    
    // Sub-states
    p3a_playback_substate_t playback_substate;
    p3a_provisioning_substate_t provisioning_substate;
    p3a_ota_substate_t ota_substate;
    
    // Channel info
    p3a_channel_info_t current_channel;
    
    // Channel message (for CHANNEL_MESSAGE sub-state)
    p3a_channel_message_t channel_message;
    
    // OTA progress
    int ota_progress_percent;
    char ota_status_text[64];
    char ota_version_from[32];
    char ota_version_to[32];
    
    // Provisioning info
    char provisioning_status[128];
    char provisioning_code[16];
    char provisioning_expires[32];
    
    // Callbacks
    struct {
        p3a_state_change_cb_t callback;
        void *user_data;
    } callbacks[MAX_CALLBACKS];
    int callback_count;
    
    // Synchronization
    SemaphoreHandle_t mutex;
    bool initialized;
} p3a_state_internal_t;

static p3a_state_internal_t s_state = {0};

// ============================================================================
// Helper Functions
// ============================================================================

static void notify_callbacks(p3a_state_t old_state, p3a_state_t new_state)
{
    for (int i = 0; i < s_state.callback_count; i++) {
        if (s_state.callbacks[i].callback) {
            s_state.callbacks[i].callback(old_state, new_state, s_state.callbacks[i].user_data);
        }
    }
}

static const char *channel_type_to_string(p3a_channel_type_t type)
{
    switch (type) {
        case P3A_CHANNEL_SDCARD: return "sdcard";
        case P3A_CHANNEL_MAKAPIX_ALL: return "all";
        case P3A_CHANNEL_MAKAPIX_PROMOTED: return "promoted";
        case P3A_CHANNEL_MAKAPIX_USER: return "user";
        case P3A_CHANNEL_MAKAPIX_BY_USER: return "by_user";
        case P3A_CHANNEL_MAKAPIX_HASHTAG: return "hashtag";
        case P3A_CHANNEL_MAKAPIX_ARTWORK: return "artwork";
        default: return "unknown";
    }
}

static p3a_channel_type_t string_to_channel_type(const char *str)
{
    if (!str) return P3A_CHANNEL_SDCARD;
    if (strcmp(str, "all") == 0) return P3A_CHANNEL_MAKAPIX_ALL;
    if (strcmp(str, "promoted") == 0) return P3A_CHANNEL_MAKAPIX_PROMOTED;
    if (strcmp(str, "user") == 0) return P3A_CHANNEL_MAKAPIX_USER;
    if (strcmp(str, "by_user") == 0) return P3A_CHANNEL_MAKAPIX_BY_USER;
    if (strcmp(str, "hashtag") == 0) return P3A_CHANNEL_MAKAPIX_HASHTAG;
    if (strcmp(str, "artwork") == 0) return P3A_CHANNEL_MAKAPIX_ARTWORK;
    return P3A_CHANNEL_SDCARD;
}

static void update_channel_display_name(p3a_channel_info_t *info)
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

esp_err_t p3a_state_persist_channel(void)
{
    if (!s_state.initialized || !s_state.mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Take mutex to read channel state atomically
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    // Copy channel info under mutex protection
    p3a_channel_info_t channel_copy;
    memcpy(&channel_copy, &s_state.current_channel, sizeof(channel_copy));
    
    xSemaphoreGive(s_state.mutex);
    
    // Don't persist transient artwork channels
    if (channel_copy.type == P3A_CHANNEL_MAKAPIX_ARTWORK) {
        return ESP_OK;
    }
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }
    
    const char *type_str = channel_type_to_string(channel_copy.type);
    err = nvs_set_str(handle, NVS_KEY_CHANNEL_TYPE, type_str);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save channel type: %s", esp_err_to_name(err));
    }
    
    if (channel_copy.type == P3A_CHANNEL_MAKAPIX_BY_USER || 
        channel_copy.type == P3A_CHANNEL_MAKAPIX_HASHTAG) {
        err = nvs_set_str(handle, NVS_KEY_CHANNEL_IDENT, channel_copy.identifier);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save channel identifier: %s", esp_err_to_name(err));
        }
    }
    
    err = nvs_commit(handle);
    nvs_close(handle);
    
    ESP_LOGI(TAG, "Persisted channel: %s", channel_copy.display_name);
    return err;
}

esp_err_t p3a_state_load_channel(p3a_channel_info_t *out_info)
{
    if (!out_info) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // No saved channel - default to SD card
        ESP_LOGI(TAG, "No saved channel, defaulting to SD card");
        out_info->type = P3A_CHANNEL_SDCARD;
        memset(out_info->identifier, 0, sizeof(out_info->identifier));
        memset(out_info->storage_key, 0, sizeof(out_info->storage_key));
        update_channel_display_name(out_info);
        return ESP_OK;
    }
    
    char type_str[32] = {0};
    size_t len = sizeof(type_str);
    err = nvs_get_str(handle, NVS_KEY_CHANNEL_TYPE, type_str, &len);
    if (err == ESP_OK) {
        out_info->type = string_to_channel_type(type_str);
    } else {
        out_info->type = P3A_CHANNEL_SDCARD;
    }
    
    if (out_info->type == P3A_CHANNEL_MAKAPIX_BY_USER || 
        out_info->type == P3A_CHANNEL_MAKAPIX_HASHTAG) {
        len = sizeof(out_info->identifier);
        err = nvs_get_str(handle, NVS_KEY_CHANNEL_IDENT, out_info->identifier, &len);
        if (err != ESP_OK) {
            // Invalid channel without identifier - fall back to SD card
            out_info->type = P3A_CHANNEL_SDCARD;
            memset(out_info->identifier, 0, sizeof(out_info->identifier));
        }
    } else {
        memset(out_info->identifier, 0, sizeof(out_info->identifier));
    }
    
    memset(out_info->storage_key, 0, sizeof(out_info->storage_key));
    update_channel_display_name(out_info);
    
    nvs_close(handle);
    
    ESP_LOGI(TAG, "Loaded channel: %s", out_info->display_name);
    return ESP_OK;
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
    
    // Load persisted channel
    p3a_state_load_channel(&s_state.current_channel);
    
    // Initialize to animation playback state
    s_state.current_state = P3A_STATE_ANIMATION_PLAYBACK;
    s_state.previous_state = P3A_STATE_ANIMATION_PLAYBACK;
    s_state.playback_substate = P3A_PLAYBACK_PLAYING;
    s_state.callback_count = 0;
    
    s_state.initialized = true;
    
    ESP_LOGI(TAG, "State machine initialized, starting in ANIMATION_PLAYBACK with channel: %s",
             s_state.current_channel.display_name);
    
    return ESP_OK;
}

void p3a_state_deinit(void)
{
    if (!s_state.initialized) return;
    
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
        case P3A_STATE_ANIMATION_PLAYBACK: return "ANIMATION_PLAYBACK";
        case P3A_STATE_PROVISIONING: return "PROVISIONING";
        case P3A_STATE_OTA: return "OTA";
        case P3A_STATE_PICO8_STREAMING: return "PICO8_STREAMING";
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
    
    notify_callbacks(old_state, P3A_STATE_ANIMATION_PLAYBACK);
    
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
    
    notify_callbacks(old_state, P3A_STATE_PROVISIONING);
    
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
    
    notify_callbacks(old_state, P3A_STATE_OTA);
    
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
    
    notify_callbacks(old_state, P3A_STATE_PICO8_STREAMING);
    
    return ESP_OK;
}

esp_err_t p3a_state_exit_to_playback(void)
{
    return p3a_state_enter_animation_playback();
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
// Channel Management
// ============================================================================

esp_err_t p3a_state_switch_channel(p3a_channel_type_t type, const char *identifier)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    
    char display_name_copy[64];
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    // Update channel info
    s_state.current_channel.type = type;
    
    if (identifier && (type == P3A_CHANNEL_MAKAPIX_BY_USER || type == P3A_CHANNEL_MAKAPIX_HASHTAG)) {
        snprintf(s_state.current_channel.identifier, sizeof(s_state.current_channel.identifier),
                 "%s", identifier);
    } else {
        memset(s_state.current_channel.identifier, 0, sizeof(s_state.current_channel.identifier));
    }
    
    memset(s_state.current_channel.storage_key, 0, sizeof(s_state.current_channel.storage_key));
    update_channel_display_name(&s_state.current_channel);
    
    // Copy display name for logging after mutex release
    snprintf(display_name_copy, sizeof(display_name_copy), "%s", s_state.current_channel.display_name);
    
    xSemaphoreGive(s_state.mutex);
    
    // Persist (except transient channels)
    p3a_state_persist_channel();
    
    ESP_LOGI(TAG, "Switched to channel: %s", display_name_copy);
    
    return ESP_OK;
}

esp_err_t p3a_state_show_artwork(const char *storage_key, const char *art_url, int32_t post_id)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (!storage_key || !art_url) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    // Create transient artwork channel
    s_state.current_channel.type = P3A_CHANNEL_MAKAPIX_ARTWORK;
    memset(s_state.current_channel.identifier, 0, sizeof(s_state.current_channel.identifier));
    snprintf(s_state.current_channel.storage_key, sizeof(s_state.current_channel.storage_key),
             "%s", storage_key);
    update_channel_display_name(&s_state.current_channel);
    
    xSemaphoreGive(s_state.mutex);
    
    // Note: Do NOT persist artwork channels - they are transient
    
    ESP_LOGI(TAG, "Showing single artwork: %s (post_id=%ld)", storage_key, (long)post_id);
    
    return ESP_OK;
}

// Forward declarations for channel player functions
extern esp_err_t channel_player_switch_to_sdcard_channel(void) __attribute__((weak));
extern esp_err_t channel_player_load_channel(void) __attribute__((weak));
extern esp_err_t animation_player_request_swap_current(void) __attribute__((weak));

esp_err_t p3a_state_fallback_to_sdcard(void)
{
    ESP_LOGI(TAG, "Falling back to SD card channel");
    
    esp_err_t err = p3a_state_switch_channel(P3A_CHANNEL_SDCARD, NULL);
    
    // Switch animation player back to sdcard_channel source
    if (channel_player_switch_to_sdcard_channel) {
        channel_player_switch_to_sdcard_channel();
    }
    if (channel_player_load_channel) {
        channel_player_load_channel();
    }
    if (animation_player_request_swap_current) {
        animation_player_request_swap_current();
    }
    
    return err;
}

p3a_channel_type_t p3a_state_get_default_channel(void)
{
    p3a_channel_info_t info;
    if (p3a_state_load_channel(&info) == ESP_OK) {
        return info.type;
    }
    return P3A_CHANNEL_SDCARD;
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

