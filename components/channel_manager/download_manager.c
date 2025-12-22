/**
 * @file download_manager.c
 * @brief Simplified download manager - single download at a time
 * 
 * Downloads files one at a time, requesting the next file from the channel
 * after each download completes. Sleeps when nothing to download.
 */

#include "download_manager.h"
#include "makapix_artwork.h"
#include "makapix_channel_events.h"
#include "sdio_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "dl_mgr";

// External: check if SD is paused for OTA
extern bool animation_player_is_sd_paused(void) __attribute__((weak));

static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_mutex = NULL;

// Callback for getting next file to download
static download_get_next_cb_t s_get_next_cb = NULL;
static void *s_get_next_ctx = NULL;

// Current download state
static char s_active_channel[64] = {0};
static bool s_busy = false;

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

static void download_task(void *arg)
{
    (void)arg;
    download_request_t req;

    while (true) {
        // Wait for prerequisites
        if (!makapix_channel_is_wifi_ready()) {
            makapix_channel_wait_for_wifi(portMAX_DELAY);
        }
        
        if (!makapix_channel_is_refresh_done()) {
            makapix_channel_wait_for_refresh(portMAX_DELAY);
        }

        if (!makapix_channel_is_sd_available()) {
            makapix_channel_wait_for_sd(portMAX_DELAY);
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

        // Get next file to download from channel
        if (!s_get_next_cb) {
            makapix_channel_wait_for_downloads_needed(portMAX_DELAY);
            continue;
        }

        memset(&req, 0, sizeof(req));
        esp_err_t get_err = s_get_next_cb(&req, s_get_next_ctx);
        
        if (get_err == ESP_ERR_NOT_FOUND) {
            makapix_channel_wait_for_downloads_needed(portMAX_DELAY);
            continue;
        }
        
        if (get_err != ESP_OK) {
            // Error getting next file - wait a bit and retry
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

        // Start download
        set_busy(true, req.channel_id);
        
        // Update UI message if no animation is playing yet
        extern void p3a_render_set_channel_message(const char *channel_name, int msg_type, int progress_percent, const char *detail);
        extern bool animation_player_is_animation_ready(void);
        if (!animation_player_is_animation_ready()) {
            p3a_render_set_channel_message("Makapix Club", 2 /* P3A_CHANNEL_MSG_DOWNLOADING */, -1, "Downloading artwork...");
        }

        char out_path[256] = {0};
        esp_err_t err = makapix_artwork_download(req.art_url, req.storage_key, out_path, sizeof(out_path));
        
        set_busy(false, NULL);

        if (err == ESP_OK) {
            makapix_channel_signal_downloads_needed();
        } else {
            if (err == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "Download not found (404): %s", req.storage_key);
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
    s_get_next_cb = NULL;
    s_get_next_ctx = NULL;
}

void download_manager_set_next_callback(download_get_next_cb_t cb, void *user_ctx)
{
    s_get_next_cb = cb;
    s_get_next_ctx = user_ctx;
    
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
