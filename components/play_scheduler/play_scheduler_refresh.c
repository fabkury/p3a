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
#include "channel_cache.h"   // For channel_cache_save()
#include "makapix.h"
#include "makapix_artwork.h"  // For makapix_artwork_download_with_progress()
#include "makapix_channel_events.h"  // For async completion events
#include "sd_path.h"
#include "p3a_state.h"   // For P3A_CHANNEL_MSG_* constants
#include "p3a_render.h"  // For p3a_render_set_channel_message()
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "ps_refresh";

// Task configuration
#define REFRESH_TASK_STACK_SIZE 8192
#define REFRESH_CHECK_INTERVAL_MS 1000

// Periodic refresh configuration
#define REFRESH_INTERVAL_SECONDS 3600  // 1 hour

// Event bits
#define REFRESH_EVENT_WORK_AVAILABLE   (1 << 0)
#define REFRESH_EVENT_SHUTDOWN         (1 << 1)

static TaskHandle_t s_refresh_task = NULL;
static EventGroupHandle_t s_refresh_events = NULL;
static volatile bool s_task_running = false;
static time_t s_last_full_refresh_complete = 0;

// PSRAM-backed stack for refresh task
static StackType_t *s_refresh_stack = NULL;
static StaticTask_t s_refresh_task_buffer;

/**
 * @brief Find next channel that needs refresh
 *
 * For Makapix channels, only returns them if MQTT is connected.
 * Artwork channels don't need MQTT - they download directly.
 * This avoids repeatedly trying to refresh when MQTT is not ready.
 *
 * @param state Scheduler state
 * @return Channel index, or -1 if none pending
 */
static int find_next_pending_refresh(ps_state_t *state)
{
    bool mqtt_ready = makapix_channel_is_mqtt_ready();

    for (size_t i = 0; i < state->channel_count; i++) {
        ps_channel_state_t *ch = &state->channels[i];
        if (!ch->refresh_pending || ch->refresh_in_progress) {
            continue;
        }

        // Artwork channels don't need MQTT - they download directly
        if (ch->type == PS_CHANNEL_TYPE_ARTWORK) {
            return (int)i;  // Always ready to process
        }

        // For Makapix channels, only proceed if MQTT is connected
        if (ch->type != PS_CHANNEL_TYPE_SDCARD && !mqtt_ready) {
            continue;  // Skip Makapix channels until MQTT is ready
        }

        return (int)i;
    }
    return -1;
}

/**
 * @brief Progress callback for artwork downloads
 *
 * Updates the UI with download progress percentage.
 */
static void artwork_download_progress_cb(size_t bytes_read, size_t content_length, void *user_ctx)
{
    (void)user_ctx;  // Unused

    int percent = 0;
    if (content_length > 0) {
        percent = (int)((bytes_read * 100) / content_length);
        if (percent > 100) percent = 100;
    }

    p3a_render_set_channel_message("Artwork", P3A_CHANNEL_MSG_DOWNLOADING, percent, NULL);
}

/**
 * @brief Refresh an artwork channel (download if needed)
 *
 * Checks if file already exists, otherwise downloads it.
 * Sets ch->active = true when file is ready for playback.
 */
static esp_err_t refresh_artwork_channel(ps_channel_state_t *ch)
{
    ESP_LOGI(TAG, "Refreshing artwork channel: %s", ch->artwork_state.storage_key);

    // Check if file already exists
    struct stat st;
    if (stat(ch->artwork_state.filepath, &st) == 0 && st.st_size > 0) {
        ESP_LOGI(TAG, "Artwork already in vault: %s", ch->artwork_state.filepath);
        ch->active = true;
        ch->artwork_state.download_pending = false;
        return ESP_OK;
    }

    // Need to download - check if we have URL
    if (ch->artwork_state.art_url[0] == '\0') {
        // Local file that doesn't exist
        ESP_LOGE(TAG, "Artwork file not found and no URL to download: %s", ch->artwork_state.filepath);
        ch->active = false;
        ch->artwork_state.download_pending = false;
        return ESP_ERR_NOT_FOUND;
    }

    // Download the artwork
    ch->artwork_state.download_in_progress = true;
    p3a_render_set_channel_message("Artwork", P3A_CHANNEL_MSG_DOWNLOADING, 0, NULL);

    ESP_LOGI(TAG, "Downloading artwork: %s", ch->artwork_state.art_url);

    char downloaded_path[256] = {0};
    esp_err_t err = makapix_artwork_download_with_progress(
        ch->artwork_state.art_url,
        ch->artwork_state.storage_key,
        downloaded_path, sizeof(downloaded_path),
        artwork_download_progress_cb, NULL
    );

    ch->artwork_state.download_in_progress = false;

    if (err == ESP_OK) {
        // Update filepath to actual downloaded path (in case it differs)
        strlcpy(ch->artwork_state.filepath, downloaded_path, sizeof(ch->artwork_state.filepath));
        ch->active = true;
        ch->artwork_state.download_pending = false;
        // Don't clear the message here - the animation player will clear it
        // after the buffer swap completes for seamless transition (no flash)
        ESP_LOGI(TAG, "Artwork download complete: %s", downloaded_path);
    } else {
        ESP_LOGE(TAG, "Artwork download failed: %s", esp_err_to_name(err));
        p3a_render_set_channel_message("Artwork", P3A_CHANNEL_MSG_ERROR, -1,
                                        esp_err_to_name(err));
        // Keep error message visible briefly
        vTaskDelay(pdMS_TO_TICKS(3000));
        p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
        ch->active = false;
        ch->artwork_state.download_pending = false;
    }

    return err;
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

    // Load the cache file into memory (160-byte sdcard_index_entry_t format)
    esp_err_t load_err = ps_load_channel_cache(ch);
    if (load_err != ESP_OK && load_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load SD card cache: %s", esp_err_to_name(load_err));
    }

    return ESP_OK;
}

/**
 * @brief Refresh a Makapix channel
 *
 * Uses the dedicated makapix_refresh_channel_index() API to trigger
 * background refresh without channel switching or navigation.
 */
static esp_err_t refresh_makapix_channel(ps_channel_state_t *ch)
{
    ESP_LOGI(TAG, "Refreshing Makapix channel: %s", ch->channel_id);

    // Parse channel_id to determine Makapix channel type and identifier
    // Channel IDs: "all", "promoted", "by_user_{sqid}", "hashtag_{tag}"

    esp_err_t err = ESP_OK;
    if (strcmp(ch->channel_id, "all") == 0) {
        err = makapix_refresh_channel_index("all", NULL);
    } else if (strcmp(ch->channel_id, "promoted") == 0) {
        err = makapix_refresh_channel_index("promoted", NULL);
    } else if (strncmp(ch->channel_id, "by_user_", 8) == 0) {
        const char *sqid = ch->channel_id + 8;
        err = makapix_refresh_channel_index("by_user", sqid);
    } else if (strncmp(ch->channel_id, "hashtag_", 8) == 0) {
        const char *tag = ch->channel_id + 8;
        err = makapix_refresh_channel_index("hashtag", tag);
    } else {
        ESP_LOGW(TAG, "Unknown Makapix channel type: %s", ch->channel_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (err == ESP_ERR_INVALID_STATE) {
        // MQTT not connected - return this error so caller can queue for retry
        ESP_LOGD(TAG, "MQTT not connected, will retry when connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to trigger Makapix refresh: %s", esp_err_to_name(err));
        return err;
    }

    // The actual refresh happens asynchronously in Makapix
    // Mark channel as waiting for async completion
    ch->refresh_async_pending = true;

    // Only load cache if not already loaded (avoid double-load during channel switch)
    // When switching channels, ps_load_channel_cache is called first, then refresh is triggered.
    // Reloading here would free the cache that was just loaded, causing heap corruption.
    if (!ch->cache_loaded) {
        // Optimistically load any existing cache (may have stale data)
        esp_err_t cache_err = ps_load_channel_cache(ch);
        if (cache_err != ESP_OK && cache_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGD(TAG, "No existing cache for '%s': %s", ch->channel_id, esp_err_to_name(cache_err));
        }
    } else {
        ESP_LOGD(TAG, "Cache already loaded for '%s', skipping reload", ch->channel_id);
    }

    // Return special code to indicate async in progress
    // The caller should NOT report completion until async finishes
    return ESP_ERR_NOT_FINISHED;
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

        // Check for async Makapix refresh completions (non-blocking poll)
        if (makapix_channel_wait_for_ps_refresh_done(0)) {
            xSemaphoreTake(state->mutex, portMAX_DELAY);
            for (size_t i = 0; i < state->channel_count; i++) {
                ps_channel_state_t *ch = &state->channels[i];
                if (ch->refresh_async_pending && makapix_ps_refresh_check_and_clear(ch->channel_id)) {
                    // This channel's async refresh completed
                    ch->refresh_async_pending = false;
                    ch->refresh_in_progress = false;

                    // Keep in-memory state - don't reload from disk.
                    // The cache was populated during batch merges and already has the correct state.
                    // Reloading would replace it with stale disk data (race with debounced save).
                    if (ch->cache) {
                        ch->cache_loaded = true;
                        ch->active = (ch->cache->available_count > 0);
                        ESP_LOGI(TAG, "Channel '%s': keeping in-memory cache (%zu entries, %zu available)",
                                 ch->channel_id, ch->cache->entry_count, ch->cache->available_count);

                        // Mark dirty to ensure eventual persistence
                        if (!ch->cache->dirty) {
                            ch->cache->dirty = true;
                        }
                    } else {
                        ESP_LOGW(TAG, "Channel '%s': no in-memory cache after refresh", ch->channel_id);
                    }

                    size_t entry_count = ch->cache ? ch->cache->entry_count : ch->entry_count;
                    ESP_LOGI(TAG, "Channel '%s' async refresh complete: %zu entries, active=%d",
                             ch->channel_id, entry_count, ch->active);

                    // Recalculate weights
                    ps_swrr_calculate_weights(state);

                    // Signal that refresh is done - this wakes download task
                    // which may be waiting for initial refresh to complete
                    makapix_channel_signal_refresh_done();

                    // Reset download cursors so download manager rescans the new cache
                    extern void download_manager_reset_cursors(void);
                    download_manager_reset_cursors();

                    // Always trigger playback after async refresh completes with entries.
                    // Note: We can't rely on animation_player_is_animation_ready() because
                    // it returns true when a message (like "No playable files available")
                    // is being displayed, which would prevent playback from starting.
                    if (entry_count > 0) {
                        ESP_LOGI(TAG, "Async refresh complete - triggering playback");

                        // Clear any loading/error message
                        extern void p3a_render_set_channel_message(const char *channel_name, int msg_type, int progress_percent, const char *detail);
                        p3a_render_set_channel_message(NULL, 0 /* P3A_CHANNEL_MSG_NONE */, -1, NULL);

                        // Signal download manager to rescan for any missing files
                        extern void download_manager_rescan(void);
                        download_manager_rescan();

                        // Release mutex before calling play_scheduler_next to avoid deadlock
                        xSemaphoreGive(state->mutex);

                        // Trigger playback - this will pick an available artwork
                        play_scheduler_next(NULL);

                        // Re-acquire mutex for the rest of the loop
                        xSemaphoreTake(state->mutex, portMAX_DELAY);
                    }
                }
            }
            xSemaphoreGive(state->mutex);
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
        } else if (type == PS_CHANNEL_TYPE_ARTWORK) {
            err = refresh_artwork_channel(ch);
        } else {
            err = refresh_makapix_channel(ch);
        }

        // Update state after refresh
        xSemaphoreTake(state->mutex, portMAX_DELAY);

        size_t sync_entry_count = 0;
        if (err == ESP_ERR_INVALID_STATE) {
            // MQTT not connected - re-queue for retry when MQTT connects
            ch->refresh_in_progress = false;
            ch->refresh_pending = true;
            ESP_LOGD(TAG, "Channel '%s' queued for retry (MQTT not connected)", channel_id);
        } else if (err == ESP_ERR_NOT_FINISHED) {
            // Makapix refresh started asynchronously - keep refresh_in_progress true
            // The async completion handler will update state when done
            ESP_LOGD(TAG, "Channel '%s' refresh started (async)", channel_id);
            // Note: refresh_async_pending was set in refresh_makapix_channel()
        } else if (err == ESP_OK) {
            ch->refresh_in_progress = false;
            sync_entry_count = ch->cache ? ch->cache->entry_count : ch->entry_count;
            ESP_LOGI(TAG, "Channel '%s' refresh complete: %zu entries, active=%d",
                     channel_id, sync_entry_count, ch->active);

            // Recalculate weights now that this channel has data
            ps_swrr_calculate_weights(state);

            // Reset download cursors so download manager rescans the new cache
            extern void download_manager_reset_cursors(void);
            download_manager_reset_cursors();

            // Signal that refresh is done - this wakes download task
            // which may be waiting for initial refresh to complete
            makapix_channel_signal_refresh_done();
        } else {
            ch->refresh_in_progress = false;
        }

        // Check if we should trigger playback after refresh
        // For artwork channels: ALWAYS trigger (show_artwork should immediately display)
        // For other channels: only trigger if no animation is currently playing
        bool should_trigger_playback = (err == ESP_OK && sync_entry_count > 0);
        bool is_artwork_channel = (type == PS_CHANNEL_TYPE_ARTWORK);

        xSemaphoreGive(state->mutex);

        if (should_trigger_playback) {
            // For artwork channels, always trigger playback immediately
            // For other channels, only trigger if nothing is playing yet
            extern bool animation_player_is_animation_ready(void);
            if (is_artwork_channel || !animation_player_is_animation_ready()) {
                ESP_LOGI(TAG, "%s - triggering playback",
                         is_artwork_channel ? "Artwork channel ready" : "No animation playing after refresh");

                // For artwork channels: DON'T clear the message here.
                // The animation player will clear it AFTER the buffer swap completes,
                // ensuring seamless UI → new animation transition (no flash).
                // For other channels: clear immediately since no animation is playing.
                if (!is_artwork_channel) {
                    p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
                }

                // Trigger playback
                play_scheduler_next(NULL);
            }
        }

        if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "Channel '%s' refresh failed: %s", channel_id, esp_err_to_name(err));
        }

        // Check if all channels have completed refresh (for periodic refresh timing)
        xSemaphoreTake(state->mutex, portMAX_DELAY);
        int pending_idx = find_next_pending_refresh(state);

        // Also check if any channel is still in async refresh
        bool any_async_pending = false;
        for (size_t i = 0; i < state->channel_count; i++) {
            if (state->channels[i].refresh_async_pending || state->channels[i].refresh_in_progress) {
                any_async_pending = true;
                break;
            }
        }

        if (pending_idx < 0 && !any_async_pending && state->channel_count > 0) {
            // All channels done (no pending AND no async in progress)
            time_t now = time(NULL);

            if (s_last_full_refresh_complete == 0) {
                // First time completing all refreshes
                s_last_full_refresh_complete = now;
                ESP_LOGI(TAG, "All channels refreshed. Next refresh in %d seconds.", REFRESH_INTERVAL_SECONDS);
            } else if (now - s_last_full_refresh_complete >= REFRESH_INTERVAL_SECONDS) {
                // Time for periodic refresh
                ESP_LOGI(TAG, "Starting periodic refresh cycle (1 hour elapsed)");
                for (size_t i = 0; i < state->channel_count; i++) {
                    state->channels[i].refresh_pending = true;
                }
                s_last_full_refresh_complete = 0;  // Reset to track next completion
                xSemaphoreGive(state->mutex);
                ps_refresh_signal_work();
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;  // Skip to next iteration
            }
        }
        xSemaphoreGive(state->mutex);

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

    // Create refresh task with SPIRAM-backed stack
    if (!s_refresh_stack) {
        s_refresh_stack = heap_caps_malloc(REFRESH_TASK_STACK_SIZE * sizeof(StackType_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    // Pin to Core 0 to avoid interfering with animation rendering on Core 1
    bool task_created = false;
    if (s_refresh_stack) {
        s_refresh_task = xTaskCreateStaticPinnedToCore(refresh_task, "ps_refresh",
                                            REFRESH_TASK_STACK_SIZE, NULL, CONFIG_P3A_APP_TASK_PRIORITY,
                                            s_refresh_stack, &s_refresh_task_buffer, 0);
        task_created = (s_refresh_task != NULL);
    }

    if (!task_created) {
        if (xTaskCreatePinnedToCore(refresh_task, "ps_refresh",
                        REFRESH_TASK_STACK_SIZE, NULL, CONFIG_P3A_APP_TASK_PRIORITY, &s_refresh_task, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create refresh task");
            return ESP_ERR_NO_MEM;
        }
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

void ps_refresh_reset_timer(void)
{
    // Reset the periodic refresh timer - called when a new scheduler command is executed
    // This ensures immediate refresh happens and the 1-hour timer starts fresh
    s_last_full_refresh_complete = 0;
    ESP_LOGD(TAG, "Refresh timer reset");
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
