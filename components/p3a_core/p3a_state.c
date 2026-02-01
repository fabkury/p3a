// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file p3a_state.c
 * @brief Unified p3a State Machine Implementation
 */

#include "p3a_state.h"
#include "esp_log.h"
#include "event_bus.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "p3a_state";

static const char *s_connectivity_short_messages[] = {
    [P3A_CONNECTIVITY_NO_WIFI] = "No Wi-Fi",
    [P3A_CONNECTIVITY_NO_INTERNET] = "No Internet",
    [P3A_CONNECTIVITY_NO_REGISTRATION] = "Not Registered",
    [P3A_CONNECTIVITY_NO_MQTT] = "Connecting...",
    [P3A_CONNECTIVITY_ONLINE] = "Online",
};

static const char *s_connectivity_detail_messages[] = {
    [P3A_CONNECTIVITY_NO_WIFI] = "Connect to Wi-Fi network",
    [P3A_CONNECTIVITY_NO_INTERNET] = "Wi-Fi connected but no internet access",
    [P3A_CONNECTIVITY_NO_REGISTRATION] = "Long-press to register with Makapix Club",
    [P3A_CONNECTIVITY_NO_MQTT] = "Connecting to Makapix Cloud",
    [P3A_CONNECTIVITY_ONLINE] = "Connected to Makapix Club",
};

// NVS storage
#define NVS_NAMESPACE "p3a_state"
#define NVS_KEY_CHANNEL_TYPE "ch_type"      // Deprecated: use playset instead
#define NVS_KEY_CHANNEL_IDENT "ch_ident"    // Deprecated: use playset instead
#define NVS_KEY_LAST_STATE "last_state"
#define NVS_KEY_ACTIVE_PLAYSET "playset"    // Active playset name (e.g., "channel_recent")

// Connectivity configuration
#define INTERNET_CHECK_INTERVAL_MS 60000
#define DNS_LOOKUP_TIMEOUT_MS 5000
#define MQTT_BACKOFF_MIN_MS 5000
#define MQTT_BACKOFF_MAX_MS 300000
#define MQTT_BACKOFF_JITTER_PERCENT 25

// Event group bits for connectivity
#define EG_BIT_ONLINE       (1 << 0)
#define EG_BIT_INTERNET     (1 << 1)
#define EG_BIT_WIFI         (1 << 2)

// Maximum callbacks
#define MAX_CALLBACKS 8

// ============================================================================
// Internal State
// ============================================================================

typedef struct {
    // Global state
    p3a_state_t current_state;
    p3a_state_t previous_state;

    // App-level status (legacy app_state)
    p3a_app_status_t app_status;

    // Sub-states
    p3a_playback_substate_t playback_substate;
    p3a_provisioning_substate_t provisioning_substate;
    p3a_ota_substate_t ota_substate;

    // Active playset name (persisted to NVS)
    char active_playset[P3A_PLAYSET_MAX_NAME_LEN + 1];

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

    // Connectivity (orthogonal)
    p3a_connectivity_level_t connectivity;
    EventGroupHandle_t connectivity_event_group;
    TimerHandle_t internet_check_timer;
    time_t last_internet_check;
    bool internet_check_in_progress;
    uint32_t mqtt_backoff_ms;
    bool has_registration;
    
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

static void update_connectivity_event_group_locked(void)
{
    if (!s_state.connectivity_event_group) {
        return;
    }

    EventBits_t bits = 0;
    switch (s_state.connectivity) {
        case P3A_CONNECTIVITY_ONLINE:
            bits = EG_BIT_ONLINE | EG_BIT_INTERNET | EG_BIT_WIFI;
            break;
        case P3A_CONNECTIVITY_NO_MQTT:
        case P3A_CONNECTIVITY_NO_REGISTRATION:
            bits = EG_BIT_INTERNET | EG_BIT_WIFI;
            break;
        case P3A_CONNECTIVITY_NO_INTERNET:
            bits = EG_BIT_WIFI;
            break;
        case P3A_CONNECTIVITY_NO_WIFI:
        default:
            bits = 0;
            break;
    }

    xEventGroupClearBits(s_state.connectivity_event_group,
                         EG_BIT_ONLINE | EG_BIT_INTERNET | EG_BIT_WIFI);
    if (bits) {
        xEventGroupSetBits(s_state.connectivity_event_group, bits);
    }
}

static void set_connectivity_locked(p3a_connectivity_level_t new_level)
{
    if (s_state.connectivity == new_level) {
        return;
    }

    ESP_LOGI(TAG, "Connectivity: %s -> %s",
             s_connectivity_short_messages[s_state.connectivity],
             s_connectivity_short_messages[new_level]);

    s_state.connectivity = new_level;
    update_connectivity_event_group_locked();
}

static bool check_registration(void)
{
    extern bool makapix_store_has_player_key(void) __attribute__((weak));
    if (makapix_store_has_player_key) {
        return makapix_store_has_player_key();
    }
    return false;
}

static void dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    bool *result = (bool *)arg;
    *result = (ipaddr != NULL);
}

static void internet_check_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (!s_state.initialized) {
        return;
    }

    bool should_check = false;
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    should_check = (s_state.connectivity == P3A_CONNECTIVITY_NO_INTERNET);
    xSemaphoreGive(s_state.mutex);

    if (should_check) {
        event_bus_emit_simple(P3A_EVENT_INTERNET_CHECK);
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
// Playset Persistence (new)
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

    // Load persisted channel (legacy, for backwards compatibility)
    p3a_state_load_channel(&s_state.current_channel);
    
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

p3a_connectivity_level_t p3a_state_get_connectivity(void)
{
    if (!s_state.initialized) return P3A_CONNECTIVITY_NO_WIFI;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    p3a_connectivity_level_t level = s_state.connectivity;
    xSemaphoreGive(s_state.mutex);

    return level;
}

const char *p3a_state_get_connectivity_message(void)
{
    p3a_connectivity_level_t level = p3a_state_get_connectivity();
    if (level < (sizeof(s_connectivity_short_messages) / sizeof(s_connectivity_short_messages[0]))) {
        return s_connectivity_short_messages[level];
    }
    return "Unknown";
}

const char *p3a_state_get_connectivity_detail(void)
{
    p3a_connectivity_level_t level = p3a_state_get_connectivity();
    if (level < (sizeof(s_connectivity_detail_messages) / sizeof(s_connectivity_detail_messages[0]))) {
        return s_connectivity_detail_messages[level];
    }
    return "Unknown state";
}

bool p3a_state_has_wifi(void)
{
    return p3a_state_get_connectivity() >= P3A_CONNECTIVITY_NO_INTERNET;
}

bool p3a_state_has_internet(void)
{
    return p3a_state_get_connectivity() >= P3A_CONNECTIVITY_NO_REGISTRATION;
}

bool p3a_state_is_online(void)
{
    return p3a_state_get_connectivity() == P3A_CONNECTIVITY_ONLINE;
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

esp_err_t p3a_state_enter_error(void)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    p3a_state_t old_state = s_state.current_state;
    s_state.previous_state = old_state;
    s_state.current_state = P3A_STATE_ERROR;

    xSemaphoreGive(s_state.mutex);

    ESP_LOGI(TAG, "State transition: %s -> ERROR", p3a_state_get_name(old_state));
    notify_callbacks(old_state, P3A_STATE_ERROR);

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
// Connectivity (orthogonal)
// ============================================================================

esp_err_t p3a_state_connectivity_init(void)
{
    if (s_state.connectivity_event_group || s_state.internet_check_timer) {
        return ESP_OK;
    }

    s_state.connectivity_event_group = xEventGroupCreate();
    if (!s_state.connectivity_event_group) {
        return ESP_ERR_NO_MEM;
    }

    s_state.internet_check_timer = xTimerCreate(
        "inet_check",
        pdMS_TO_TICKS(INTERNET_CHECK_INTERVAL_MS),
        pdTRUE,
        NULL,
        internet_check_timer_cb
    );
    if (!s_state.internet_check_timer) {
        vEventGroupDelete(s_state.connectivity_event_group);
        s_state.connectivity_event_group = NULL;
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.connectivity = P3A_CONNECTIVITY_NO_WIFI;
    s_state.last_internet_check = 0;
    s_state.internet_check_in_progress = false;
    s_state.mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;
    s_state.has_registration = check_registration();
    update_connectivity_event_group_locked();
    xSemaphoreGive(s_state.mutex);

    ESP_LOGI(TAG, "Connectivity initialized (registration=%d)", s_state.has_registration);
    return ESP_OK;
}

void p3a_state_connectivity_deinit(void)
{
    if (s_state.internet_check_timer) {
        xTimerStop(s_state.internet_check_timer, portMAX_DELAY);
        xTimerDelete(s_state.internet_check_timer, portMAX_DELAY);
        s_state.internet_check_timer = NULL;
    }
    if (s_state.connectivity_event_group) {
        vEventGroupDelete(s_state.connectivity_event_group);
        s_state.connectivity_event_group = NULL;
    }
}

void p3a_state_on_wifi_connected(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    set_connectivity_locked(P3A_CONNECTIVITY_NO_INTERNET);
    if (s_state.internet_check_timer) {
        xTimerStart(s_state.internet_check_timer, 0);
    }
    xSemaphoreGive(s_state.mutex);

    p3a_state_check_internet();
}

void p3a_state_on_wifi_disconnected(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    if (s_state.internet_check_timer) {
        xTimerStop(s_state.internet_check_timer, 0);
    }
    set_connectivity_locked(P3A_CONNECTIVITY_NO_WIFI);
    s_state.mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;
    xSemaphoreGive(s_state.mutex);
}

void p3a_state_on_mqtt_connected(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    set_connectivity_locked(P3A_CONNECTIVITY_ONLINE);
    s_state.mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;
    xSemaphoreGive(s_state.mutex);
}

void p3a_state_on_mqtt_disconnected(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    if (s_state.connectivity >= P3A_CONNECTIVITY_NO_MQTT) {
        s_state.has_registration = check_registration();
        if (s_state.has_registration) {
            set_connectivity_locked(P3A_CONNECTIVITY_NO_MQTT);
        } else {
            set_connectivity_locked(P3A_CONNECTIVITY_NO_REGISTRATION);
        }

        s_state.mqtt_backoff_ms = s_state.mqtt_backoff_ms * 2;
        if (s_state.mqtt_backoff_ms > MQTT_BACKOFF_MAX_MS) {
            s_state.mqtt_backoff_ms = MQTT_BACKOFF_MAX_MS;
        }

        uint32_t jitter = (s_state.mqtt_backoff_ms * MQTT_BACKOFF_JITTER_PERCENT) / 100;
        uint32_t rand_val = esp_random() % (jitter * 2);
        s_state.mqtt_backoff_ms = s_state.mqtt_backoff_ms - jitter + rand_val;
    }

    xSemaphoreGive(s_state.mutex);
}

void p3a_state_on_registration_changed(bool has_registration)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.has_registration = has_registration;

    if (s_state.connectivity == P3A_CONNECTIVITY_NO_REGISTRATION && has_registration) {
        set_connectivity_locked(P3A_CONNECTIVITY_NO_MQTT);
    } else if (s_state.connectivity >= P3A_CONNECTIVITY_NO_MQTT && !has_registration) {
        set_connectivity_locked(P3A_CONNECTIVITY_NO_REGISTRATION);
    }

    xSemaphoreGive(s_state.mutex);
}

bool p3a_state_check_internet(void)
{
    if (!s_state.initialized) return false;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    if (s_state.internet_check_in_progress) {
        xSemaphoreGive(s_state.mutex);
        return s_state.connectivity >= P3A_CONNECTIVITY_NO_REGISTRATION;
    }
    s_state.internet_check_in_progress = true;
    xSemaphoreGive(s_state.mutex);

    ESP_LOGD(TAG, "Checking internet via DNS lookup...");
    ip_addr_t addr;
    volatile bool dns_done = false;
    volatile bool dns_success = false;

    err_t err = dns_gethostbyname("example.com", &addr,
                                   (dns_found_callback)dns_callback,
                                   (void *)&dns_success);

    if (err == ERR_OK) {
        dns_success = true;
        dns_done = true;
    } else if (err == ERR_INPROGRESS) {
        TickType_t start = xTaskGetTickCount();
        while (!dns_done &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(DNS_LOOKUP_TIMEOUT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (dns_success) {
                dns_done = true;
                break;
            }
        }
    }

    if (!dns_success) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                if (ip_info.ip.addr != 0 && ip_info.gw.addr != 0) {
                    dns_success = true;
                    ESP_LOGD(TAG, "DNS failed but have IP - assuming internet OK");
                }
            }
        }
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.internet_check_in_progress = false;

    if (dns_success) {
        s_state.last_internet_check = time(NULL);
        if (s_state.connectivity == P3A_CONNECTIVITY_NO_INTERNET) {
            s_state.has_registration = check_registration();
            if (s_state.has_registration) {
                set_connectivity_locked(P3A_CONNECTIVITY_NO_MQTT);
            } else {
                set_connectivity_locked(P3A_CONNECTIVITY_NO_REGISTRATION);
            }
        }
        ESP_LOGI(TAG, "Internet check: OK");
    } else {
        if (s_state.connectivity > P3A_CONNECTIVITY_NO_INTERNET) {
            set_connectivity_locked(P3A_CONNECTIVITY_NO_INTERNET);
        }
        ESP_LOGW(TAG, "Internet check: FAILED");
    }

    bool result = (s_state.connectivity >= P3A_CONNECTIVITY_NO_REGISTRATION);
    xSemaphoreGive(s_state.mutex);
    return result;
}

uint32_t p3a_state_get_last_internet_check_age(void)
{
    if (!s_state.initialized || s_state.last_internet_check == 0) {
        return UINT32_MAX;
    }

    time_t now = time(NULL);
    if (now < s_state.last_internet_check) {
        return 0;
    }

    return (uint32_t)(now - s_state.last_internet_check);
}

esp_err_t p3a_state_wait_for_online(TickType_t timeout_ms)
{
    if (!s_state.initialized || !s_state.connectivity_event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_state.connectivity_event_group,
        EG_BIT_ONLINE,
        pdFALSE,
        pdTRUE,
        timeout_ms
    );

    return (bits & EG_BIT_ONLINE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t p3a_state_wait_for_internet(TickType_t timeout_ms)
{
    if (!s_state.initialized || !s_state.connectivity_event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_state.connectivity_event_group,
        EG_BIT_INTERNET,
        pdFALSE,
        pdTRUE,
        timeout_ms
    );

    return (bits & EG_BIT_INTERNET) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t p3a_state_wait_for_wifi(TickType_t timeout_ms)
{
    if (!s_state.initialized || !s_state.connectivity_event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_state.connectivity_event_group,
        EG_BIT_WIFI,
        pdFALSE,
        pdTRUE,
        timeout_ms
    );

    return (bits & EG_BIT_WIFI) ? ESP_OK : ESP_ERR_TIMEOUT;
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

// Forward declarations for play_scheduler functions
extern esp_err_t play_scheduler_play_named_channel(const char *name) __attribute__((weak));
extern esp_err_t play_scheduler_refresh_sdcard_cache(void) __attribute__((weak));

// External UI function declarations
extern void p3a_render_set_channel_message(const char *channel_name, int msg_type, int progress_percent, const char *detail) __attribute__((weak));
extern esp_err_t ugfx_ui_show_channel_message(const char *channel_name, const char *message, int progress_percent) __attribute__((weak));

esp_err_t p3a_state_fallback_to_sdcard(void)
{
    ESP_LOGI(TAG, "Falling back to SD card channel");

    esp_err_t err = p3a_state_switch_channel(P3A_CHANNEL_SDCARD, NULL);

    // Switch play_scheduler to sdcard channel
    if (play_scheduler_play_named_channel) {
        esp_err_t ps_err = play_scheduler_play_named_channel("sdcard");
        if (ps_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to switch play_scheduler to sdcard: %s", esp_err_to_name(ps_err));
        }
    }
    if (play_scheduler_refresh_sdcard_cache) {
        play_scheduler_refresh_sdcard_cache();
    }

    // Check if SD card channel has any artworks
    extern bool animation_player_is_animation_ready(void) __attribute__((weak));

    bool has_animations = false;
    if (animation_player_is_animation_ready) {
        has_animations = animation_player_is_animation_ready();
    }

    if (!has_animations) {
        // SD card is also empty - show persistent "no artworks" message
        ESP_LOGW(TAG, "No artworks available on SD card either - showing empty message");
        if (p3a_render_set_channel_message) {
            p3a_render_set_channel_message("p3a", 4 /* P3A_CHANNEL_MSG_EMPTY */, -1,
                                          "No artworks available.\nLong-press to register.");
        }
        if (ugfx_ui_show_channel_message) {
            ugfx_ui_show_channel_message("p3a", "No artworks available.\nLong-press to register.", -1);
        }
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

