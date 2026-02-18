// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file makapix_refresh.c
 * @brief Background channel index refresh for Play Scheduler
 */

#include "makapix_internal.h"

// ---------------------------------------------------------------------------
// Background channel index refresh (for Play Scheduler)
// ---------------------------------------------------------------------------

// Track background refresh handles to avoid recreating them repeatedly
static channel_handle_t s_refresh_handle_all = NULL;
static channel_handle_t s_refresh_handle_promoted = NULL;

// Track user/hashtag refresh handles for cancellation
#define MAX_TRACKED_REFRESH_HANDLES 4
static channel_handle_t s_tracked_refresh_handles[MAX_TRACKED_REFRESH_HANDLES] = {NULL};
static size_t s_tracked_refresh_count = 0;

// ---------------------------------------------------------------------------
// Cancel all active refresh tasks
// ---------------------------------------------------------------------------

esp_err_t makapix_cancel_all_refreshes(void)
{
    ESP_LOGI(MAKAPIX_TAG, "Cancelling all active refresh tasks");

    // Cancel "all" channel refresh
    if (s_refresh_handle_all) {
        makapix_channel_stop_refresh(s_refresh_handle_all);
    }

    // Cancel "promoted" channel refresh
    if (s_refresh_handle_promoted) {
        makapix_channel_stop_refresh(s_refresh_handle_promoted);
    }

    // Cancel tracked user/hashtag handles
    for (size_t i = 0; i < s_tracked_refresh_count; i++) {
        if (s_tracked_refresh_handles[i]) {
            makapix_channel_stop_refresh(s_tracked_refresh_handles[i]);
            // Destroy the temporary handle after stopping
            channel_destroy(s_tracked_refresh_handles[i]);
            s_tracked_refresh_handles[i] = NULL;
        }
    }
    s_tracked_refresh_count = 0;

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Play Scheduler refresh completion tracking
// ---------------------------------------------------------------------------

#define MAX_PS_PENDING_REFRESH 8

typedef struct {
    char channel_id[64];
    bool completed;
} ps_refresh_pending_t;

static ps_refresh_pending_t s_ps_pending_refreshes[MAX_PS_PENDING_REFRESH] = {0};
static SemaphoreHandle_t s_ps_pending_mutex = NULL;

void makapix_ps_refresh_register(const char *channel_id)
{
    if (!channel_id) return;

    // Create mutex on first use
    if (!s_ps_pending_mutex) {
        s_ps_pending_mutex = xSemaphoreCreateMutex();
        if (!s_ps_pending_mutex) {
            ESP_LOGE(MAKAPIX_TAG, "Failed to create PS refresh mutex");
            return;
        }
    }

    xSemaphoreTake(s_ps_pending_mutex, portMAX_DELAY);

    // Check if already registered
    for (int i = 0; i < MAX_PS_PENDING_REFRESH; i++) {
        if (strcmp(s_ps_pending_refreshes[i].channel_id, channel_id) == 0) {
            // Already registered, reset completion flag
            s_ps_pending_refreshes[i].completed = false;
            xSemaphoreGive(s_ps_pending_mutex);
            ESP_LOGD(MAKAPIX_TAG, "PS refresh re-registered: %s", channel_id);
            return;
        }
    }

    // Find empty slot
    for (int i = 0; i < MAX_PS_PENDING_REFRESH; i++) {
        if (s_ps_pending_refreshes[i].channel_id[0] == '\0') {
            strlcpy(s_ps_pending_refreshes[i].channel_id, channel_id,
                    sizeof(s_ps_pending_refreshes[i].channel_id));
            s_ps_pending_refreshes[i].completed = false;
            xSemaphoreGive(s_ps_pending_mutex);
            ESP_LOGD(MAKAPIX_TAG, "PS refresh registered: %s", channel_id);
            return;
        }
    }

    xSemaphoreGive(s_ps_pending_mutex);
    ESP_LOGW(MAKAPIX_TAG, "PS refresh table full, cannot register: %s", channel_id);
}

void makapix_ps_refresh_mark_complete(const char *channel_id)
{
    if (!channel_id || !s_ps_pending_mutex) return;

    xSemaphoreTake(s_ps_pending_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_PS_PENDING_REFRESH; i++) {
        if (strcmp(s_ps_pending_refreshes[i].channel_id, channel_id) == 0) {
            s_ps_pending_refreshes[i].completed = true;
            xSemaphoreGive(s_ps_pending_mutex);
            ESP_LOGI(MAKAPIX_TAG, "PS refresh complete: %s", channel_id);
            // Signal Play Scheduler
            makapix_channel_signal_ps_refresh_done(channel_id);
            return;
        }
    }

    xSemaphoreGive(s_ps_pending_mutex);
    // Not registered - that's OK, may have been triggered by non-PS path
}

bool makapix_ps_refresh_check_and_clear(const char *channel_id)
{
    if (!channel_id || !s_ps_pending_mutex) return false;

    xSemaphoreTake(s_ps_pending_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_PS_PENDING_REFRESH; i++) {
        if (strcmp(s_ps_pending_refreshes[i].channel_id, channel_id) == 0 &&
            s_ps_pending_refreshes[i].completed) {
            // Clear the entry
            s_ps_pending_refreshes[i].channel_id[0] = '\0';
            s_ps_pending_refreshes[i].completed = false;
            xSemaphoreGive(s_ps_pending_mutex);
            return true;
        }
    }

    xSemaphoreGive(s_ps_pending_mutex);
    return false;
}

// ---------------------------------------------------------------------------

esp_err_t makapix_refresh_channel_index(const char *channel_type, const char *identifier)
{
    if (!channel_type) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check MQTT connection
    if (!makapix_mqtt_is_connected()) {
        ESP_LOGW(MAKAPIX_TAG, "Cannot refresh channel: MQTT not connected");
        return ESP_ERR_INVALID_STATE;
    }

    // Build channel_id from type and identifier
    char channel_id[128] = {0};
    char channel_name[64] = {0};

    if (strcmp(channel_type, "all") == 0) {
        strncpy(channel_id, "all", sizeof(channel_id) - 1);
        strncpy(channel_name, "All", sizeof(channel_name) - 1);
    } else if (strcmp(channel_type, "promoted") == 0) {
        strncpy(channel_id, "promoted", sizeof(channel_id) - 1);
        strncpy(channel_name, "Promoted", sizeof(channel_name) - 1);
    } else if (strcmp(channel_type, "by_user") == 0 && identifier) {
        snprintf(channel_id, sizeof(channel_id), "by_user_%s", identifier);
        snprintf(channel_name, sizeof(channel_name), "User %s", identifier);
    } else if (strcmp(channel_type, "hashtag") == 0 && identifier) {
        snprintf(channel_id, sizeof(channel_id), "hashtag_%s", identifier);
        snprintf(channel_name, sizeof(channel_name), "#%s", identifier);
    } else {
        ESP_LOGW(MAKAPIX_TAG, "Unknown channel type: %s", channel_type);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(MAKAPIX_TAG, "Refreshing channel index: %s (no channel switch)", channel_id);

    // Register for Play Scheduler completion tracking
    makapix_ps_refresh_register(channel_id);

    // Get paths
    char vault_path[128] = {0};
    char channels_path[128] = {0};
    if (sd_path_get_vault(vault_path, sizeof(vault_path)) != ESP_OK) {
        snprintf(vault_path, sizeof(vault_path), "%s/vault", SD_PATH_DEFAULT_ROOT);
    }
    if (sd_path_get_channel(channels_path, sizeof(channels_path)) != ESP_OK) {
        snprintf(channels_path, sizeof(channels_path), "%s/channel", SD_PATH_DEFAULT_ROOT);
    }

    // Check if we already have a handle for this channel type (reuse for "all" and "promoted")
    channel_handle_t handle = NULL;
    if (strcmp(channel_type, "all") == 0) {
        if (!s_refresh_handle_all) {
            s_refresh_handle_all = makapix_channel_create(channel_id, channel_name, vault_path, channels_path);
        }
        handle = s_refresh_handle_all;
    } else if (strcmp(channel_type, "promoted") == 0) {
        if (!s_refresh_handle_promoted) {
            s_refresh_handle_promoted = makapix_channel_create(channel_id, channel_name, vault_path, channels_path);
        }
        handle = s_refresh_handle_promoted;
    } else {
        // For user/hashtag channels, create a handle and track it for cancellation
        handle = makapix_channel_create(channel_id, channel_name, vault_path, channels_path);
        if (!handle) {
            ESP_LOGE(MAKAPIX_TAG, "Failed to create channel for refresh: %s", channel_id);
            return ESP_ERR_NO_MEM;
        }

        // Track this handle for later cancellation
        if (s_tracked_refresh_count < MAX_TRACKED_REFRESH_HANDLES) {
            s_tracked_refresh_handles[s_tracked_refresh_count++] = handle;
            ESP_LOGD(MAKAPIX_TAG, "Tracking refresh handle for %s (slot %zu)",
                     channel_id, s_tracked_refresh_count - 1);
        } else {
            // Array full - stop oldest handle to make room
            ESP_LOGW(MAKAPIX_TAG, "Refresh handle tracking full, stopping oldest");
            if (s_tracked_refresh_handles[0]) {
                makapix_channel_stop_refresh(s_tracked_refresh_handles[0]);
                channel_destroy(s_tracked_refresh_handles[0]);
            }
            // Shift array
            for (size_t i = 0; i < MAX_TRACKED_REFRESH_HANDLES - 1; i++) {
                s_tracked_refresh_handles[i] = s_tracked_refresh_handles[i + 1];
            }
            s_tracked_refresh_handles[MAX_TRACKED_REFRESH_HANDLES - 1] = handle;
        }

        // Load to trigger the refresh task
        esp_err_t err = channel_load(handle);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(MAKAPIX_TAG, "Channel load/refresh failed: %s", esp_err_to_name(err));
            // Remove from tracking and destroy
            for (size_t i = 0; i < s_tracked_refresh_count; i++) {
                if (s_tracked_refresh_handles[i] == handle) {
                    s_tracked_refresh_handles[i] = s_tracked_refresh_handles[--s_tracked_refresh_count];
                    s_tracked_refresh_handles[s_tracked_refresh_count] = NULL;
                    break;
                }
            }
            channel_destroy(handle);
            return err;
        }
        return ESP_OK;
    }

    if (!handle) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to create/get channel handle for refresh: %s", channel_id);
        return ESP_ERR_NO_MEM;
    }

    // Trigger refresh via channel_load (which starts the refresh task if needed)
    esp_err_t err = channel_load(handle);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(MAKAPIX_TAG, "Channel load/refresh failed: %s", esp_err_to_name(err));
        return err;
    }

    // Additionally, explicitly request refresh if the channel is already loaded but not yet refreshing.
    // This ensures the refresh task queries for new data even if cache already exists.
    // Skip if already refreshing (channel_load may have started it).
    if (!makapix_channel_is_refreshing(handle)) {
        channel_request_refresh(handle);
    }

    ESP_LOGD(MAKAPIX_TAG, "Refresh initiated for %s (background)", channel_id);
    return ESP_OK;
}
