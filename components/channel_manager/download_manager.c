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
 * - Reads cache files directly to find entries needing download
 * - Sleeps when nothing to download
 */

#include "download_manager.h"
#include "play_scheduler.h"
#include "makapix_channel_impl.h"
#include "makapix_channel_utils.h"
#include "makapix_artwork.h"
#include "makapix_channel_events.h"
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
// Download Manager State (decoupled from Play Scheduler)
// ============================================================================

#define DL_MAX_CHANNELS 16

typedef struct {
    char channel_id[64];
    uint32_t dl_cursor;          // Current position for scanning
    bool channel_complete;       // All entries scanned this epoch
} dl_channel_state_t;

static dl_channel_state_t s_dl_channels[DL_MAX_CHANNELS];
static size_t s_dl_channel_count = 0;
static size_t s_dl_round_robin_idx = 0;

static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_mutex = NULL;

// Current download state
static char s_active_channel[64] = {0};
static bool s_busy = false;
static bool s_playback_initiated = false;  // Track if we've started playback

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

static bool has_404_marker(const char *filepath)
{
    if (!filepath || filepath[0] == '\0') return false;
    char marker_path[264];
    int ret = snprintf(marker_path, sizeof(marker_path), "%s.404", filepath);
    if (ret < 0 || ret >= (int)sizeof(marker_path)) return false;
    struct stat st;
    return (stat(marker_path, &st) == 0);
}

/**
 * @brief Build cache file path for a channel
 */
static void dl_build_cache_path(const char *channel_id, char *out_path, size_t max_len)
{
    char channel_dir[256];
    if (sd_path_get_channel(channel_dir, sizeof(channel_dir)) != ESP_OK) {
        strlcpy(channel_dir, "/sdcard/p3a/channel", sizeof(channel_dir));
    }

    // Replace : with _ in filename
    char safe_id[64];
    size_t j = 0;
    for (size_t i = 0; channel_id[i] && j < sizeof(safe_id) - 1; i++) {
        safe_id[j++] = (channel_id[i] == ':') ? '_' : channel_id[i];
    }
    safe_id[j] = '\0';

    snprintf(out_path, max_len, "%s/%s.bin", channel_dir, safe_id);
}

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
    char vault_base[128];
    if (sd_path_get_vault(vault_base, sizeof(vault_base)) != ESP_OK) {
        strlcpy(vault_base, "/sdcard/p3a/vault", sizeof(vault_base));
    }

    // Convert UUID bytes to string
    char storage_key[40];
    bytes_to_uuid(entry->storage_key_uuid, storage_key, sizeof(storage_key));

    // Compute SHA256 for sharding
    uint8_t sha256[32];
    if (storage_key_sha256(storage_key, sha256) != ESP_OK) {
        // Fallback without sharding
        int ext_idx = (entry->extension <= 3) ? entry->extension : 0;
        snprintf(out, out_len, "%s/%s%s", vault_base, storage_key, s_ext_strings[ext_idx]);
        return;
    }

    // Build sharded path
    int ext_idx = (entry->extension <= 3) ? entry->extension : 0;
    snprintf(out, out_len, "%s/%02x/%02x/%02x/%s%s",
             vault_base,
             (unsigned int)sha256[0],
             (unsigned int)sha256[1],
             (unsigned int)sha256[2],
             storage_key,
             s_ext_strings[ext_idx]);
}

/**
 * @brief Get next download using round-robin across channels
 *
 * Scans channels in round-robin order, finding the first entry that:
 * - Is an artwork (not playlist)
 * - Does not have a local file
 * - Does not have a .404 marker
 *
 * @param out_request Output download request
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if all files downloaded
 */
static esp_err_t dl_get_next_download(download_request_t *out_request)
{
    if (s_dl_channel_count == 0 || !out_request) {
        return ESP_ERR_NOT_FOUND;
    }

    // Try each channel in round-robin order
    for (size_t attempt = 0; attempt < s_dl_channel_count; attempt++) {
        size_t ch_idx = (s_dl_round_robin_idx + attempt) % s_dl_channel_count;
        dl_channel_state_t *ch = &s_dl_channels[ch_idx];

        // Skip SD card channel (no downloads needed)
        if (strcmp(ch->channel_id, "sdcard") == 0) {
            continue;
        }

        // Skip completed channels
        if (ch->channel_complete) {
            continue;
        }

        // Build cache path and open file
        char cache_path[512];
        dl_build_cache_path(ch->channel_id, cache_path, sizeof(cache_path));

        struct stat st;
        if (stat(cache_path, &st) != 0) {
            // No cache file yet
            continue;
        }

        if (st.st_size <= 0 || st.st_size % 64 != 0) {
            // Invalid cache file
            continue;
        }

        size_t entry_count = st.st_size / 64;

        // Open and seek to cursor position
        FILE *f = fopen(cache_path, "rb");
        if (!f) {
            continue;
        }

        // Scan from cursor position
        makapix_channel_entry_t entry;
        bool found = false;

        while (ch->dl_cursor < entry_count) {
            // Seek to cursor position
            if (fseek(f, ch->dl_cursor * sizeof(makapix_channel_entry_t), SEEK_SET) != 0) {
                break;
            }

            if (fread(&entry, sizeof(entry), 1, f) != 1) {
                break;
            }

            ch->dl_cursor++;

            // Skip non-artwork entries
            if (entry.kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) {
                continue;
            }

            // Build filepath
            char filepath[256];
            dl_build_vault_filepath(&entry, filepath, sizeof(filepath));

            // Skip if file exists
            if (file_exists(filepath)) {
                continue;
            }

            // Skip if 404 marker exists
            if (has_404_marker(filepath)) {
                continue;
            }

            // Found entry needing download
            char storage_key[40];
            bytes_to_uuid(entry.storage_key_uuid, storage_key, sizeof(storage_key));

            memset(out_request, 0, sizeof(*out_request));
            strlcpy(out_request->storage_key, storage_key, sizeof(out_request->storage_key));
            strlcpy(out_request->filepath, filepath, sizeof(out_request->filepath));
            strlcpy(out_request->channel_id, ch->channel_id, sizeof(out_request->channel_id));

            // Build artwork URL
            uint8_t sha256[32] = {0};
            if (storage_key_sha256(storage_key, sha256) == ESP_OK) {
                int ext_idx = (entry.extension <= 3) ? entry.extension : 0;
                snprintf(out_request->art_url, sizeof(out_request->art_url),
                         "https://%s/api/vault/%02x/%02x/%02x/%s%s",
                         CONFIG_MAKAPIX_CLUB_HOST,
                         (unsigned int)sha256[0], (unsigned int)sha256[1], (unsigned int)sha256[2],
                         storage_key, s_ext_strings[ext_idx]);
            }

            found = true;
            break;
        }

        fclose(f);

        if (found) {
            // Advance round-robin to next channel for fairness
            s_dl_round_robin_idx = (ch_idx + 1) % s_dl_channel_count;
            ESP_LOGD(TAG, "Download: ch=%s key=%s", ch->channel_id, out_request->storage_key);
            return ESP_OK;
        }

        // This channel is exhausted
        ch->channel_complete = true;
        ESP_LOGI(TAG, "Channel '%s' download scan complete", ch->channel_id);
    }

    // All channels complete
    return ESP_ERR_NOT_FOUND;
}

static void download_task(void *arg)
{
    (void)arg;
    download_request_t req;

    ESP_LOGI(TAG, "Download task started");

    while (true) {
        // Wait for prerequisites
        if (!makapix_channel_is_wifi_ready()) {
            ESP_LOGI(TAG, "Waiting for WiFi...");
            makapix_channel_wait_for_wifi(portMAX_DELAY);
            ESP_LOGI(TAG, "WiFi ready");
        }

        if (!makapix_channel_is_refresh_done()) {
            ESP_LOGI(TAG, "Waiting for channel refresh...");
            makapix_channel_wait_for_refresh(portMAX_DELAY);
            ESP_LOGI(TAG, "Channel refresh done");
        }

        if (!makapix_channel_is_sd_available()) {
            ESP_LOGI(TAG, "Waiting for SD card...");
            makapix_channel_wait_for_sd(portMAX_DELAY);
            ESP_LOGI(TAG, "SD card available");
        }

        // Wait if SDIO bus is locked or SD access is paused
        int wait_count = 0;
        const int max_wait = 30;
        while (wait_count < max_wait) {
            bool should_wait = sdio_bus_is_locked();
            if (!should_wait && animation_player_is_sd_paused) {
                should_wait = animation_player_is_sd_paused();
            }
            if (!should_wait) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
            wait_count++;
        }

        // Get next file to download using own round-robin logic
        memset(&req, 0, sizeof(req));
        esp_err_t get_err = ESP_ERR_NOT_FOUND;

        if (s_dl_channel_count > 0) {
            get_err = dl_get_next_download(&req);
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
        if (req.storage_key[0] == '\0' || req.art_url[0] == '\0') {
            ESP_LOGW(TAG, "Invalid download request (empty storage_key or url)");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Check if file already exists (race condition protection)
        if (file_exists(req.filepath)) {
            ESP_LOGD(TAG, "File already exists: %s", req.storage_key);
            // Signal that we should check for next file immediately
            makapix_channel_signal_downloads_needed();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Delete any existing temp file
        if (req.filepath[0]) {
            char temp_path[260];
            snprintf(temp_path, sizeof(temp_path), "%s.tmp", req.filepath);
            struct stat tmp_st;
            if (stat(temp_path, &tmp_st) == 0) {
                ESP_LOGD(TAG, "Removing orphan temp file: %s", temp_path);
                unlink(temp_path);
            }
        }

        // Ensure cache doesn't grow unbounded - evict old files if needed
        // Files from active channels are protected by recent mtime (touched on play)
        // Use limit of 1000 items (~200MB at 200KB average per artwork)
        makapix_artwork_ensure_cache_limit(1000);

        // Start download
        set_busy(true, req.channel_id);

        // Update UI message if no animation is playing yet AND we haven't initiated playback
        // This ensures the message only shows during initial boot, not on subsequent downloads
        extern void p3a_render_set_channel_message(const char *channel_name, int msg_type, int progress_percent, const char *detail);
        extern bool animation_player_is_animation_ready(void);
        if (!s_playback_initiated && !animation_player_is_animation_ready()) {
            p3a_render_set_channel_message("Makapix Club", 2 /* P3A_CHANNEL_MSG_DOWNLOADING */, -1, "Downloading artwork...");
        }

        char out_path[256] = {0};
        esp_err_t err = makapix_artwork_download(req.art_url, req.storage_key, out_path, sizeof(out_path));
        
        set_busy(false, NULL);

        if (err == ESP_OK) {
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
                ESP_LOGW(TAG, "Download not found (404): %s", req.storage_key);
                // Create .404 marker with timestamp to prevent retry
                char marker_path[520];
                snprintf(marker_path, sizeof(marker_path), "%s.404", req.filepath);
                FILE *f = fopen(marker_path, "w");
                if (f) {
                    time_t now = time(NULL);
                    fprintf(f, "%ld\n", (long)now);
                    fclose(f);
                    ESP_LOGI(TAG, "Created 404 marker: %s (timestamp=%ld)", marker_path, (long)now);
                }
            } else {
                ESP_LOGW(TAG, "Download failed (%s): %s", esp_err_to_name(err), req.storage_key);
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
    
    s_playback_initiated = false;  // Reset on init

    if (xTaskCreate(download_task, "download_mgr", 16384, NULL, 5, &s_task) != pdPASS) {
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
