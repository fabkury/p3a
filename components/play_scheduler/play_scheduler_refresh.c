// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_refresh.c
 * @brief Background channel refresh task for Play Scheduler
 *
 * Sequentially refreshes channels that have `refresh_pending` set.
 * For SD card channels: rebuilds the sdcard.bin index
 * For Makapix channels: triggers the existing Makapix refresh mechanism
 */

#include "play_scheduler_internal.h"
#include "play_scheduler.h"  // For play_scheduler_next()
#include "makapix.h"
#include "sd_path.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "ps_refresh";

// Task configuration
#define REFRESH_TASK_STACK_SIZE 4096
#define REFRESH_TASK_PRIORITY   5
#define REFRESH_CHECK_INTERVAL_MS 1000

// Event bits
#define REFRESH_EVENT_WORK_AVAILABLE   (1 << 0)
#define REFRESH_EVENT_SHUTDOWN         (1 << 1)

static TaskHandle_t s_refresh_task = NULL;
static EventGroupHandle_t s_refresh_events = NULL;
static volatile bool s_task_running = false;

/**
 * @brief Find next channel that needs refresh
 *
 * @param state Scheduler state
 * @return Channel index, or -1 if none pending
 */
static int find_next_pending_refresh(ps_state_t *state)
{
    for (size_t i = 0; i < state->channel_count; i++) {
        if (state->channels[i].refresh_pending && !state->channels[i].refresh_in_progress) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Refresh an SD card channel
 *
 * Rebuilds the sdcard.bin index by scanning the animations folder.
 */
static esp_err_t refresh_sdcard_channel(ps_channel_state_t *ch)
{
    ESP_LOGI(TAG, "Refreshing SD card channel");

    esp_err_t err = ps_build_sdcard_index();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build SD card index: %s", esp_err_to_name(err));
        return err;
    }

    // Reload the cache to get new entry count
    // The cache was just built, so ps_load_channel_cache should find it
    // We need to re-read the cache stats
    char channel_path[256];
    ps_build_cache_path_internal(ch->channel_id, channel_path, sizeof(channel_path));

    struct stat st;
    if (stat(channel_path, &st) == 0 && st.st_size > 0) {
        ch->entry_count = st.st_size / 64;
        ch->cache_loaded = true;
        ch->active = (ch->entry_count > 0);
    }

    return ESP_OK;
}

/**
 * @brief Refresh a Makapix channel
 *
 * For now, this triggers the Makapix refresh by switching to the channel.
 * In the future, we could call the makapix_api_query_posts directly.
 */
static esp_err_t refresh_makapix_channel(ps_channel_state_t *ch)
{
    ESP_LOGI(TAG, "Refreshing Makapix channel: %s", ch->channel_id);

    // Parse channel_id to determine Makapix channel type
    // Channel IDs: "all", "promoted", "user:{sqid}", "hashtag:{tag}"

    if (strcmp(ch->channel_id, "all") == 0) {
        // Request channel switch which will trigger refresh
        makapix_request_channel_switch("all", NULL);
    } else if (strcmp(ch->channel_id, "promoted") == 0) {
        makapix_request_channel_switch("promoted", NULL);
    } else if (strncmp(ch->channel_id, "user:", 5) == 0) {
        const char *sqid = ch->channel_id + 5;
        makapix_request_channel_switch("by_user", sqid);
    } else if (strncmp(ch->channel_id, "hashtag:", 8) == 0) {
        const char *tag = ch->channel_id + 8;
        makapix_request_channel_switch("hashtag", tag);
    } else {
        ESP_LOGW(TAG, "Unknown Makapix channel type: %s", ch->channel_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // The actual refresh happens asynchronously in Makapix
    // We just mark it as no longer pending here
    // The cache will be updated by the Makapix refresh task

    // TODO: In a more complete implementation, we would:
    // 1. Wait for Makapix refresh to complete
    // 2. Reload the cache to get updated entry count
    // For now, we'll reload the cache optimistically

    char cache_path[512];
    ps_build_cache_path_internal(ch->channel_id, cache_path, sizeof(cache_path));

    struct stat st;
    if (stat(cache_path, &st) == 0 && st.st_size > 0) {
        ch->entry_count = st.st_size / 64;
        ch->cache_loaded = true;
        ch->active = (ch->entry_count > 0);
    }

    return ESP_OK;
}

/**
 * @brief Background refresh task
 *
 * Runs continuously, processing pending channel refreshes one at a time.
 */
static void refresh_task(void *arg)
{
    ps_state_t *state = ps_get_state();

    ESP_LOGI(TAG, "Refresh task started");
    s_task_running = true;

    while (s_task_running) {
        // Wait for work or shutdown
        EventBits_t bits = xEventGroupWaitBits(
            s_refresh_events,
            REFRESH_EVENT_WORK_AVAILABLE | REFRESH_EVENT_SHUTDOWN,
            pdTRUE,  // Clear bits on exit
            pdFALSE, // Wait for any bit
            pdMS_TO_TICKS(REFRESH_CHECK_INTERVAL_MS)
        );

        if (bits & REFRESH_EVENT_SHUTDOWN) {
            ESP_LOGI(TAG, "Shutdown requested");
            break;
        }

        // Find next pending refresh
        xSemaphoreTake(state->mutex, portMAX_DELAY);
        int ch_idx = find_next_pending_refresh(state);

        if (ch_idx < 0) {
            xSemaphoreGive(state->mutex);
            continue;
        }

        ps_channel_state_t *ch = &state->channels[ch_idx];
        ch->refresh_in_progress = true;
        ch->refresh_pending = false;

        // Copy what we need before releasing mutex
        ps_channel_type_t type = ch->type;
        char channel_id[64];
        strlcpy(channel_id, ch->channel_id, sizeof(channel_id));

        xSemaphoreGive(state->mutex);

        // Perform refresh (outside mutex to avoid blocking scheduler)
        esp_err_t err = ESP_OK;

        if (type == PS_CHANNEL_TYPE_SDCARD) {
            err = refresh_sdcard_channel(ch);
        } else {
            err = refresh_makapix_channel(ch);
        }

        // Update state after refresh
        xSemaphoreTake(state->mutex, portMAX_DELAY);

        ch->refresh_in_progress = false;

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Channel '%s' refresh complete: %zu entries, active=%d",
                     channel_id, ch->entry_count, ch->active);

            // Recalculate weights now that this channel has data
            ps_swrr_calculate_weights(state);
        }

        // Check if we should trigger initial playback (no animation playing yet)
        bool should_trigger_playback = (err == ESP_OK && ch->entry_count > 0);

        xSemaphoreGive(state->mutex);

        if (should_trigger_playback) {
            // Check if animation is playing (must be done outside mutex to avoid deadlock)
            extern bool animation_player_is_animation_ready(void);
            if (!animation_player_is_animation_ready()) {
                ESP_LOGI(TAG, "No animation playing after refresh - triggering playback");
                // Clear the loading message
                extern void p3a_render_set_channel_message(const char *channel_name, int msg_type, int progress_percent, const char *detail);
                p3a_render_set_channel_message(NULL, 0 /* P3A_CHANNEL_MSG_NONE */, -1, NULL);

                // Trigger playback
                play_scheduler_next(NULL);
            }
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Channel '%s' refresh failed: %s", channel_id, esp_err_to_name(err));
        }

        // Brief delay between refreshes to avoid overloading
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Refresh task exiting");
    s_refresh_task = NULL;
    vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t ps_refresh_start(void)
{
    if (s_refresh_task != NULL) {
        ESP_LOGD(TAG, "Refresh task already running");
        return ESP_OK;
    }

    if (s_refresh_events == NULL) {
        s_refresh_events = xEventGroupCreate();
        if (s_refresh_events == NULL) {
            ESP_LOGE(TAG, "Failed to create event group");
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t ret = xTaskCreate(
        refresh_task,
        "ps_refresh",
        REFRESH_TASK_STACK_SIZE,
        NULL,
        REFRESH_TASK_PRIORITY,
        &s_refresh_task
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create refresh task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Refresh task created");
    return ESP_OK;
}

void ps_refresh_stop(void)
{
    if (s_refresh_task == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Stopping refresh task");
    s_task_running = false;

    if (s_refresh_events) {
        xEventGroupSetBits(s_refresh_events, REFRESH_EVENT_SHUTDOWN);
    }

    // Wait for task to exit (with timeout)
    for (int i = 0; i < 50 && s_refresh_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_refresh_events) {
        vEventGroupDelete(s_refresh_events);
        s_refresh_events = NULL;
    }
}

void ps_refresh_signal_work(void)
{
    if (s_refresh_events) {
        xEventGroupSetBits(s_refresh_events, REFRESH_EVENT_WORK_AVAILABLE);
    }
}

// Helper to build cache path (exposed for use by refresh_task)
void ps_build_cache_path_internal(const char *channel_id, char *out_path, size_t max_len)
{
    char channel_dir[256];
    if (sd_path_get_channel(channel_dir, sizeof(channel_dir)) != ESP_OK) {
        strlcpy(channel_dir, "/sdcard/p3a/channel", sizeof(channel_dir));
    }

    // Replace : with _ for filesystem safety
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
