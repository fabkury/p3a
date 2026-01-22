// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file download_manager.c
 * @brief Decoupled download manager with own state
 *
 * Downloads files one at a time using round-robin across channels.
 * Owns its own channel list and download cursors - fully decoupled from
 * Play Scheduler (no lookahead dependency).
 *
 * Architecture:
 * - Receives channel list via download_manager_set_channels()
 * - Round-robin across channels to find next missing file
 * - Uses channel_cache APIs to find entries needing download
 * - Sleeps when nothing to download
 */

#include "download_manager.h"
#include "play_scheduler.h"
#include "esp_heap_caps.h"
#include "makapix_channel_impl.h"
#include "makapix_channel_utils.h"
#include "makapix_artwork.h"
#include "makapix_channel_events.h"
#include "channel_cache.h"
#include "load_tracker.h"
#include "sd_path.h"
#include "sdio_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "dl_mgr";

// External: check if SD is paused for OTA
extern bool animation_player_is_sd_paused(void) __attribute__((weak));

// ============================================================================
// Helpers - Display Name
// ============================================================================

/**
 * @brief Get user-friendly display name from channel_id
 */
static void dl_get_display_name(const char *channel_id, char *out_name, size_t max_len)
{
    if (!channel_id || !out_name || max_len == 0) return;
    
    if (strcmp(channel_id, "all") == 0) {
        snprintf(out_name, max_len, "All Artworks");
    } else if (strcmp(channel_id, "promoted") == 0) {
        snprintf(out_name, max_len, "Promoted");
    } else if (strcmp(channel_id, "user") == 0) {
        snprintf(out_name, max_len, "My Channel");
    } else if (strcmp(channel_id, "sdcard") == 0) {
        snprintf(out_name, max_len, "microSD Card");
    } else if (strncmp(channel_id, "by_user_", 8) == 0) {
        // Truncate user ID to fit in output buffer with "User: " prefix
        snprintf(out_name, max_len, "User: %.48s", channel_id + 8);
    } else if (strncmp(channel_id, "hashtag_", 8) == 0) {
        // Truncate hashtag to fit in output buffer with "#" prefix
        snprintf(out_name, max_len, "#%.56s", channel_id + 8);
    } else {
        // Truncate channel_id to fit in output buffer
        snprintf(out_name, max_len, "%.63s", channel_id);
    }
}

// ============================================================================
// Download Manager State (decoupled from Play Scheduler)
// ============================================================================

#define DL_MAX_CHANNELS 16

typedef struct {
    char channel_id[64];
    uint32_t dl_cursor;          // Current position for scanning
    bool channel_complete;       // All entries scanned this epoch
} dl_channel_state_t;

/**
 * @brief Snapshot of channel state for thread-safe operation
 *
 * Used by dl_get_next_download() to operate on a consistent copy of
 * channel state without holding the mutex during file operations.
 */
typedef struct {
    dl_channel_state_t channels[DL_MAX_CHANNELS];
    size_t channel_count;
    size_t round_robin_idx;
} dl_snapshot_t;

static dl_channel_state_t s_dl_channels[DL_MAX_CHANNELS];
static size_t s_dl_channel_count = 0;
static size_t s_dl_round_robin_idx = 0;

static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_mutex = NULL;

// Current download state
static char s_active_channel[64] = {0};
static bool s_busy = false;
static bool s_playback_initiated = false;  // Track if we've started playback

// PSRAM-backed static task stack for reduced internal RAM usage
static StackType_t *s_download_stack = NULL;
static StaticTask_t s_download_task_buffer;
static bool s_download_stack_allocated = false;

static bool file_exists(const char *path)
{
    if (!path || !*path) return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

static void set_busy(bool busy, const char *channel_id)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_busy = busy;
        if (busy && channel_id && channel_id[0]) {
            strlcpy(s_active_channel, channel_id, sizeof(s_active_channel));
        } else if (!busy) {
            s_active_channel[0] = '\0';
        }
        xSemaphoreGive(s_mutex);
    } else {
        s_busy = busy;
    }
}

// Static buffer for 404 marker check (single-threaded access from download task)
static char s_marker_path[264];

static bool has_404_marker(const char *filepath)
{
    if (!filepath || filepath[0] == '\0') return false;
    int ret = snprintf(s_marker_path, sizeof(s_marker_path), "%s.404", filepath);
    if (ret < 0 || ret >= (int)sizeof(s_marker_path)) return false;
    struct stat st;
    return (stat(s_marker_path, &st) == 0);
}

// Static buffers for dl_build_vault_filepath to reduce stack usage
// Safe because only one download task exists (single-threaded access)
static char s_build_vault_base[128];
static char s_build_storage_key[40];
static uint8_t s_build_sha256[32];

/**
 * @brief Build vault filepath for an entry
 *
 * Uses SHA256 sharding: {vault}/{sha[0]}/{sha[1]}/{sha[2]}/{storage_key}.{ext}
 */
static void dl_build_vault_filepath(const makapix_channel_entry_t *entry,
                                    char *out, size_t out_len)
{
    if (!entry || !out || out_len == 0) {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }

    // Get vault base path
    if (sd_path_get_vault(s_build_vault_base, sizeof(s_build_vault_base)) != ESP_OK) {
        strlcpy(s_build_vault_base, "/sdcard/p3a/vault", sizeof(s_build_vault_base));
    }

    // Convert UUID bytes to string
    bytes_to_uuid(entry->storage_key_uuid, s_build_storage_key, sizeof(s_build_storage_key));

    // Compute SHA256 for sharding
    if (storage_key_sha256(s_build_storage_key, s_build_sha256) != ESP_OK) {
        // Fallback without sharding
        int ext_idx = (entry->extension <= 3) ? entry->extension : 0;
        snprintf(out, out_len, "%s/%s%s", s_build_vault_base, s_build_storage_key, s_ext_strings[ext_idx]);
        return;
    }

    // Build sharded path
    int ext_idx = (entry->extension <= 3) ? entry->extension : 0;
    snprintf(out, out_len, "%s/%02x/%02x/%02x/%s%s",
             s_build_vault_base,
             (unsigned int)s_build_sha256[0],
             (unsigned int)s_build_sha256[1],
             (unsigned int)s_build_sha256[2],
             s_build_storage_key,
             s_ext_strings[ext_idx]);
}

// ============================================================================
// Snapshot Functions for Thread-Safe Channel Access
// ============================================================================

/**
 * @brief Take snapshot of channel state under mutex
 *
 * Creates a local copy of channel state to avoid holding mutex during
 * file operations. This prevents race conditions with download_manager_set_channels().
 *
 * @param out_snapshot Output snapshot structure
 * @return true if snapshot was taken successfully, false if no channels or error
 */
static bool dl_take_snapshot(dl_snapshot_t *out_snapshot)
{
    if (!out_snapshot) return false;

    bool success = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_dl_channel_count > 0) {
            out_snapshot->channel_count = s_dl_channel_count;
            out_snapshot->round_robin_idx = s_dl_round_robin_idx;
            memcpy(out_snapshot->channels, s_dl_channels,
                   s_dl_channel_count * sizeof(dl_channel_state_t));
            success = true;
        }
        xSemaphoreGive(s_mutex);
    }

    return success;
}

/**
 * @brief Commit round-robin index and cursor changes back to shared state
 *
 * Only commits if the channel list hasn't changed since snapshot was taken.
 * If channels changed, the new channel list takes precedence and our changes
 * are discarded (the new channel switch reset everything anyway).
 *
 * @param snapshot The snapshot used for the current operation (with updated cursors)
 * @param new_round_robin_idx The new round-robin index to commit
 */
static void dl_commit_state(const dl_snapshot_t *snapshot, size_t new_round_robin_idx)
{
    if (!snapshot) return;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Only commit if channel count hasn't changed
        // This indicates no channel switch occurred during our operation
        if (s_dl_channel_count == snapshot->channel_count) {
            s_dl_round_robin_idx = new_round_robin_idx;

            // Also update cursor positions for channels we scanned
            for (size_t i = 0; i < snapshot->channel_count; i++) {
                // Find matching channel by ID and update its state
                for (size_t j = 0; j < s_dl_channel_count; j++) {
                    if (strcmp(s_dl_channels[j].channel_id, snapshot->channels[i].channel_id) == 0) {
                        s_dl_channels[j].dl_cursor = snapshot->channels[i].dl_cursor;
                        s_dl_channels[j].channel_complete = snapshot->channels[i].channel_complete;
                        break;
                    }
                }
            }
        }
        xSemaphoreGive(s_mutex);
    }
}

// Static buffers for dl_get_next_download to reduce stack usage
// Safe because only one download task exists (single-threaded access)
static char s_dl_filepath[256];
static char s_dl_vault_base[128];
static char s_dl_storage_key[40];
static uint8_t s_dl_sha256[32];

/**
 * @brief Get next download using round-robin across channels
 *
 * Operates on a snapshot of channel state to avoid race conditions.
 * Uses channel_cache APIs to find entries not yet in LAi (Locally Available index).
 * Scans channels in round-robin order, finding the first entry that:
 * - Is an artwork (not playlist) and not in LAi
 * - Does not have a .404 marker
 * - Is not blocked by LTF (load-to-failure tracking)
 *
 * @param out_request Output download request
 * @param snapshot Snapshot of channel state (will be modified with cursor updates)
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if all files downloaded
 */
static esp_err_t dl_get_next_download(download_request_t *out_request, dl_snapshot_t *snapshot)
{
    if (!snapshot || snapshot->channel_count == 0 || !out_request) {
        return ESP_ERR_NOT_FOUND;
    }

    // Try each channel in round-robin order
    for (size_t attempt = 0; attempt < snapshot->channel_count; attempt++) {
        size_t ch_idx = (snapshot->round_robin_idx + attempt) % snapshot->channel_count;
        dl_channel_state_t *ch = &snapshot->channels[ch_idx];

        // Skip non-Makapix channels (e.g., SD card - no downloads needed)
        if (!play_scheduler_is_makapix_channel(ch->channel_id)) {
            continue;
        }

        // Skip completed channels
        if (ch->channel_complete) {
            continue;
        }

        // Get channel cache from registry
        channel_cache_t *cache = channel_cache_registry_find(ch->channel_id);
        if (!cache) {
            ESP_LOGD(TAG, "Cache not found for '%s'", ch->channel_id);
            continue;
        }
        ESP_LOGD(TAG, "Checking cache '%s': entry_count=%zu available=%zu",
                 ch->channel_id, cache->entry_count, cache->available_count);

        // Find entries needing download (artwork not in LAi)
        makapix_channel_entry_t entry;
        bool found = false;

        while (channel_cache_get_next_missing(cache, &ch->dl_cursor, &entry) == ESP_OK) {
            // Build filepath into static buffer (used by has_404_marker and output)
            dl_build_vault_filepath(&entry, s_dl_filepath, sizeof(s_dl_filepath));

            // Skip if 404 marker exists
            if (has_404_marker(s_dl_filepath)) {
                continue;
            }

            // Get vault base path for LTF check
            if (sd_path_get_vault(s_dl_vault_base, sizeof(s_dl_vault_base)) != ESP_OK) {
                strlcpy(s_dl_vault_base, "/sdcard/p3a/vault", sizeof(s_dl_vault_base));
            }

            // Convert UUID to string (use local entry, not stale static)
            bytes_to_uuid(entry.storage_key_uuid, s_dl_storage_key, sizeof(s_dl_storage_key));

            // Skip if LTF is terminal (3 load failures)
            if (!ltf_can_download(s_dl_storage_key, s_dl_vault_base)) {
                ESP_LOGD(TAG, "Skipping terminal LTF: %s", s_dl_storage_key);
                continue;
            }

            // Found entry needing download
            memset(out_request, 0, sizeof(*out_request));
            strlcpy(out_request->storage_key, s_dl_storage_key, sizeof(out_request->storage_key));
            strlcpy(out_request->filepath, s_dl_filepath, sizeof(out_request->filepath));
            strlcpy(out_request->channel_id, ch->channel_id, sizeof(out_request->channel_id));

            // Build artwork URL (using static sha256 buffer)
            memset(s_dl_sha256, 0, sizeof(s_dl_sha256));
            if (storage_key_sha256(s_dl_storage_key, s_dl_sha256) == ESP_OK) {
                int ext_idx = (entry.extension <= 3) ? entry.extension : 0;
                snprintf(out_request->art_url, sizeof(out_request->art_url),
                         "https://%s/api/vault/%02x/%02x/%02x/%s%s",
                         CONFIG_MAKAPIX_CLUB_HOST,
                         (unsigned int)s_dl_sha256[0], (unsigned int)s_dl_sha256[1], (unsigned int)s_dl_sha256[2],
                         s_dl_storage_key, s_ext_strings[ext_idx]);
                // ESP_LOGI(TAG, "Download URL: %s (ext_idx=%d)", out_request->art_url, ext_idx);
            }

            found = true;
            break;
        }

        if (found) {
            // Advance round-robin to next channel for fairness and commit to shared state
            size_t new_rr_idx = (ch_idx + 1) % snapshot->channel_count;
            dl_commit_state(snapshot, new_rr_idx);
            ESP_LOGD(TAG, "Download: ch=%s key=%s", ch->channel_id, out_request->storage_key);
            return ESP_OK;
        }

        // This channel is exhausted
        ch->channel_complete = true;
        ESP_LOGI(TAG, "Channel '%s' download scan complete", ch->channel_id);
    }

    // All channels complete - commit the completion state
    dl_commit_state(snapshot, snapshot->round_robin_idx);
    return ESP_ERR_NOT_FOUND;
}

// Static buffers to reduce stack usage in download_task
// These are safe because only one download task instance exists
static download_request_t s_dl_req;
static dl_snapshot_t s_dl_snapshot;
static char s_task_temp_path[264];    // For temp file checks
static char s_task_out_path[256];     // For download output path
static char s_task_vault_base[128];   // For LTF operations
static char s_task_marker_path[264];  // For 404 marker creation
static char s_task_display_name[64];  // For UI display name

static void download_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Download task started");

    while (true) {
        // Wait for prerequisites - WiFi and SD card must be available
        // NOTE: We no longer wait for full refresh completion. The refresh task
        // signals downloads_needed after each batch, allowing early downloads
        // as soon as the first batch of index entries arrives.
        if (!makapix_channel_is_wifi_ready()) {
            ESP_LOGI(TAG, "Waiting for WiFi...");
            makapix_channel_wait_for_wifi(portMAX_DELAY);
            ESP_LOGI(TAG, "WiFi ready");
        }

        if (!makapix_channel_is_sd_available()) {
            ESP_LOGI(TAG, "Waiting for SD card...");
            makapix_channel_wait_for_sd(portMAX_DELAY);
            ESP_LOGI(TAG, "SD card available");
        }

        // Wait if SDIO bus is locked or SD access is paused
        int wait_count = 0;
        const int max_wait = 120;  // Wait up to 120 seconds
        while (wait_count < max_wait) {
            bool should_wait = sdio_bus_is_locked();
            if (!should_wait && animation_player_is_sd_paused) {
                should_wait = animation_player_is_sd_paused();
            }
            if (!should_wait) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
            wait_count++;
        }

        if (wait_count >= max_wait) {
            const char *holder = sdio_bus_get_holder();
            ESP_LOGW(TAG, "SDIO bus still locked by %s after %d seconds, skipping download cycle",
                     holder ? holder : "unknown", max_wait);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;  // Skip this cycle
        }

        // Get next file to download using own round-robin logic
        // Take a snapshot of channel state under mutex to avoid race conditions
        // Note: Using static buffers (s_dl_req, s_dl_snapshot) to reduce stack usage
        memset(&s_dl_req, 0, sizeof(s_dl_req));
        esp_err_t get_err = ESP_ERR_NOT_FOUND;

        memset(&s_dl_snapshot, 0, sizeof(s_dl_snapshot));
        if (dl_take_snapshot(&s_dl_snapshot)) {
            get_err = dl_get_next_download(&s_dl_req, &s_dl_snapshot);
            if (get_err == ESP_OK) {
                // Clear the signal since we have work - prevents race condition where
                // signal is set while we're busy and gets cleared before we see it
                makapix_channel_clear_downloads_needed();
            }
        }

        if (get_err == ESP_ERR_NOT_FOUND) {
            // All files downloaded OR no channels configured - wait for signal
            // Clear signal before waiting so we only wake on NEW signals
            if (s_dl_channel_count == 0) {
                ESP_LOGD(TAG, "No channels configured, waiting for signal...");
            } else {
                ESP_LOGI(TAG, "All files downloaded, waiting for signal...");
            }
            makapix_channel_clear_downloads_needed();
            makapix_channel_wait_for_downloads_needed(portMAX_DELAY);
            ESP_LOGI(TAG, "Woke from downloads_needed wait");
            continue;
        }

        if (get_err != ESP_OK) {
            // Error getting next file
            ESP_LOGW(TAG, "Error getting next download: %s", esp_err_to_name(get_err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Validate request
        if (s_dl_req.storage_key[0] == '\0' || s_dl_req.art_url[0] == '\0') {
            ESP_LOGW(TAG, "Invalid download request (empty storage_key or url)");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Check if file already exists (race condition protection)
        if (file_exists(s_dl_req.filepath)) {
            ESP_LOGD(TAG, "File already exists: %s", s_dl_req.storage_key);
            // Signal that we should check for next file immediately
            makapix_channel_signal_downloads_needed();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Delete any existing temp file (using static buffer)
        if (s_dl_req.filepath[0]) {
            snprintf(s_task_temp_path, sizeof(s_task_temp_path), "%s.tmp", s_dl_req.filepath);
            struct stat tmp_st;
            if (stat(s_task_temp_path, &tmp_st) == 0) {
                ESP_LOGD(TAG, "Removing orphan temp file: %s", s_task_temp_path);
                unlink(s_task_temp_path);
            }
        }

        // Ensure cache doesn't grow unbounded - evict old files if needed
        // Files from active channels are protected by recent mtime (touched on play)
        // Use limit of 1000 items (~200MB at 200KB average per artwork)
        makapix_artwork_ensure_cache_limit(1000);

        // Start download
        set_busy(true, s_dl_req.channel_id);

        // Update UI message if:
        // - No animation is playing yet
        // - We haven't initiated playback
        // - The initial refresh has completed (so we don't override "Updating channel index...")
        // During initial refresh, let the refresh task control the message
        extern void p3a_render_set_channel_message(const char *channel_name, int msg_type, int progress_percent, const char *detail);
        extern bool animation_player_is_animation_ready(void);
        bool refresh_done = makapix_channel_is_refresh_done();
        if (!s_playback_initiated && !animation_player_is_animation_ready() && refresh_done) {
            // Get display name from channel_id in the request (using static buffer)
            dl_get_display_name(s_dl_req.channel_id, s_task_display_name, sizeof(s_task_display_name));
            p3a_render_set_channel_message(s_task_display_name, 2 /* P3A_CHANNEL_MSG_DOWNLOADING */, -1, "Downloading artwork...");
        }

        memset(s_task_out_path, 0, sizeof(s_task_out_path));
        esp_err_t err = makapix_artwork_download(s_dl_req.art_url, s_dl_req.storage_key, s_task_out_path, sizeof(s_task_out_path));
        
        set_busy(false, NULL);

        if (err == ESP_OK) {
            // Clear any previous LTF failures for this file (using static buffer)
            if (sd_path_get_vault(s_task_vault_base, sizeof(s_task_vault_base)) != ESP_OK) {
                strlcpy(s_task_vault_base, "/sdcard/p3a/vault", sizeof(s_task_vault_base));
            }
            ltf_clear(s_dl_req.storage_key, s_task_vault_base);

            // Signal play_scheduler to update LAi
            // The play_scheduler will find the ci_index and add to LAi
            play_scheduler_on_download_complete(s_dl_req.channel_id, s_dl_req.storage_key);

            makapix_channel_signal_downloads_needed();
            makapix_channel_signal_file_available();  // Wake tasks waiting for first file

            // Check if we should trigger initial playback (first file downloaded during boot)
            extern bool animation_player_is_animation_ready(void);
            if (!animation_player_is_animation_ready() && !s_playback_initiated) {
                // No animation playing yet - try to start playback via play_scheduler
                esp_err_t swap_err = play_scheduler_next(NULL);
                if (swap_err == ESP_OK) {
                    ESP_LOGI(TAG, "First download complete - triggered playback via play_scheduler");
                    s_playback_initiated = true;  // Mark that we've initiated playback
                    // Clear the loading message since playback is starting
                    p3a_render_set_channel_message(NULL, 0 /* P3A_CHANNEL_MSG_NONE */, -1, NULL);
                } else {
                    ESP_LOGD(TAG, "play_scheduler_next after download returned: %s", esp_err_to_name(swap_err));
                }
            }
        } else {
            if (err == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "Download not found (404): %s", s_dl_req.storage_key);
                // Create .404 marker with timestamp to prevent retry (using static buffer)
                snprintf(s_task_marker_path, sizeof(s_task_marker_path), "%s.404", s_dl_req.filepath);
                FILE *f = fopen(s_task_marker_path, "w");
                if (f) {
                    time_t now = time(NULL);
                    fprintf(f, "%ld\n", (long)now);
                    fclose(f);
                    ESP_LOGI(TAG, "Created 404 marker: %s (timestamp=%ld)", s_task_marker_path, (long)now);
                }
            } else {
                ESP_LOGW(TAG, "Download failed (%s): %s", esp_err_to_name(err), s_dl_req.storage_key);
            }
            // Wait a bit before trying next file
            vTaskDelay(pdMS_TO_TICKS(1000));
            // Still signal to check for next file
            makapix_channel_signal_downloads_needed();
        }

        // Brief delay between downloads to reduce SDIO bus contention
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t download_manager_init(void)
{
    if (s_task) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_playback_initiated = false;

    const size_t stack_size = 81920;

    // Try PSRAM first for the 80KB stack
    s_download_stack = heap_caps_malloc(stack_size * sizeof(StackType_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_download_stack) {
        s_download_stack_allocated = true;
        s_task = xTaskCreateStatic(download_task, "download_mgr",
                                   stack_size, NULL, 3,
                                   s_download_stack, &s_download_task_buffer);
        if (s_task) {
            ESP_LOGI(TAG, "Download manager task using PSRAM stack");
            return ESP_OK;
        }
        heap_caps_free(s_download_stack);
        s_download_stack = NULL;
        s_download_stack_allocated = false;
    }

    // Fallback to dynamic allocation (internal RAM)
    ESP_LOGW(TAG, "PSRAM stack unavailable, using internal RAM");
    if (xTaskCreate(download_task, "download_mgr", stack_size, NULL, 3, &s_task) != pdPASS) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void download_manager_deinit(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_download_stack_allocated && s_download_stack) {
        heap_caps_free(s_download_stack);
        s_download_stack = NULL;
        s_download_stack_allocated = false;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
}

void download_manager_set_next_callback(download_get_next_cb_t cb, void *user_ctx)
{
    // Legacy API - no longer used, downloads are now driven by Play Scheduler
    // Kept for API compatibility
    (void)cb;
    (void)user_ctx;

    // If callback is set, signal that we should check for downloads
    if (cb) {
        makapix_channel_signal_downloads_needed();
    }
}

void download_manager_signal_work_available(void)
{
    // Reset channel_complete flags so download manager will re-scan
    // This is needed because channels may be marked complete before batches arrive
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (size_t i = 0; i < s_dl_channel_count; i++) {
            s_dl_channels[i].channel_complete = false;
        }
        xSemaphoreGive(s_mutex);
    }

    makapix_channel_signal_downloads_needed();
}

bool download_manager_is_busy(void)
{
    bool busy = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        busy = s_busy;
        xSemaphoreGive(s_mutex);
    } else {
        busy = s_busy;
    }
    return busy;
}

bool download_manager_get_active_channel(char *out_channel_id, size_t max_len)
{
    if (!out_channel_id || max_len == 0) return false;

    bool has_active = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_active_channel[0]) {
            strlcpy(out_channel_id, s_active_channel, max_len);
            has_active = true;
        }
        xSemaphoreGive(s_mutex);
    }
    return has_active;
}

// ============================================================================
// Decoupled Channel Management API
// ============================================================================

void download_manager_set_channels(const char **channel_ids, size_t count)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_dl_channel_count = (count < DL_MAX_CHANNELS) ? count : DL_MAX_CHANNELS;
        s_dl_round_robin_idx = 0;

        for (size_t i = 0; i < s_dl_channel_count; i++) {
            strlcpy(s_dl_channels[i].channel_id, channel_ids[i], sizeof(s_dl_channels[i].channel_id));
            s_dl_channels[i].dl_cursor = 0;
            s_dl_channels[i].channel_complete = false;
        }

        ESP_LOGI(TAG, "Configured %zu channel(s) for download", s_dl_channel_count);
        xSemaphoreGive(s_mutex);
    }

    // Signal that we should check for downloads
    makapix_channel_signal_downloads_needed();
}

void download_manager_reset_cursors(void)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (size_t i = 0; i < s_dl_channel_count; i++) {
            s_dl_channels[i].dl_cursor = 0;
            s_dl_channels[i].channel_complete = false;
        }
        s_dl_round_robin_idx = 0;
        ESP_LOGI(TAG, "Reset download cursors");
        xSemaphoreGive(s_mutex);
    }
}

void download_manager_reset_playback_initiated(void)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_playback_initiated = false;
        ESP_LOGD(TAG, "Reset playback_initiated flag");
        xSemaphoreGive(s_mutex);
    } else {
        s_playback_initiated = false;
    }
}
