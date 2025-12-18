#include "download_manager.h"
#include "makapix_artwork.h"
#include "sdio_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "dl_mgr";

extern bool animation_player_is_sd_paused(void) __attribute__((weak));

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_mutex = NULL;

// Channel cancellation tracking
static char s_cancelled_channel[64] = {0};
static char s_active_channel[64] = {0};

// Completion callback
static download_completion_cb_t s_completion_cb = NULL;
static void *s_completion_ctx = NULL;

// Increased queue length for batch downloads (32 artworks per batch + buffer)
#define DL_QUEUE_LEN 64

static bool file_exists(const char *path)
{
    if (!path || !*path) return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

// Check if downloads for a channel should be skipped (cancelled)
static bool is_channel_cancelled(const char *channel_id)
{
    if (!channel_id || !channel_id[0]) return false;
    
    bool cancelled = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cancelled = (s_cancelled_channel[0] != '\0' && 
                     strcmp(s_cancelled_channel, channel_id) == 0);
        xSemaphoreGive(s_mutex);
    }
    return cancelled;
}

// Set the active channel being downloaded
static void set_active_channel(const char *channel_id)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (channel_id && channel_id[0]) {
            strlcpy(s_active_channel, channel_id, sizeof(s_active_channel));
        } else {
            s_active_channel[0] = '\0';
        }
        xSemaphoreGive(s_mutex);
    }
}

static void download_task(void *arg)
{
    (void)arg;
    download_request_t req;

    while (true) {
        if (!s_queue) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Check if this channel's downloads were cancelled
        if (is_channel_cancelled(req.channel_id)) {
            ESP_LOGD(TAG, "Skipping cancelled download: %s (channel=%s)", 
                     req.storage_key, req.channel_id);
            continue;
        }

        // Track active channel
        set_active_channel(req.channel_id);

        // If already downloaded, just update playlist availability if applicable.
        if (file_exists(req.filepath)) {
            if (req.playlist_post_id != 0 && req.artwork_post_id != 0) {
                playlist_update_artwork_status(req.playlist_post_id, req.artwork_post_id, true);
            }
            // Still invoke callback for already-existing files (they're "available")
            if (s_completion_cb && req.filepath[0]) {
                s_completion_cb(req.channel_id, req.storage_key, req.filepath, s_completion_ctx);
            }
            set_active_channel(NULL);
            continue;
        }

        // Only Makapix-style downloads for now (needs URL + storage_key).
        if (req.storage_key[0] == '\0' || req.art_url[0] == '\0') {
            ESP_LOGW(TAG, "Skipping download (missing storage_key/art_url)");
            set_active_channel(NULL);
            continue;
        }

        // Delete any existing temp file before downloading (per spec: handle partial downloads)
        if (req.filepath[0]) {
            char temp_path[260];
            snprintf(temp_path, sizeof(temp_path), "%s.tmp", req.filepath);
            struct stat tmp_st;
            if (stat(temp_path, &tmp_st) == 0) {
                ESP_LOGD(TAG, "Removing orphan temp file before download: %s", temp_path);
                unlink(temp_path);
            }
        }

        // Retry with backoff: 1s, 5s, 15s
        const int backoff_ms[] = { 1000, 5000, 15000 };
        const int max_attempts = 3;
        bool download_success = false;

        for (int attempt = 0; attempt < max_attempts; attempt++) {
            // Check cancellation before each attempt
            if (is_channel_cancelled(req.channel_id)) {
                ESP_LOGD(TAG, "Download cancelled mid-retry: %s", req.storage_key);
                break;
            }

            // Wait if SDIO bus is locked or SD access is paused (e.g., OTA)
            int wait_count = 0;
            const int max_wait = 30;
            while (wait_count < max_wait) {
                bool should_wait = sdio_bus_is_locked();
                if (!should_wait && animation_player_is_sd_paused) {
                    should_wait = animation_player_is_sd_paused();
                }
                if (!should_wait) break;
                if (wait_count == 0) {
                    const char *holder = sdio_bus_get_holder();
                    ESP_LOGI(TAG, "SDIO busy (%s), waiting before download...",
                             holder ? holder : "SD paused");
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                wait_count++;
            }

            char out_path[256] = {0};
            esp_err_t err = makapix_artwork_download(req.art_url, req.storage_key, out_path, sizeof(out_path));
            if (err == ESP_OK) {
                // Prefer the actual downloaded path as filepath for future checks.
                if (out_path[0] != '\0') {
                    strlcpy(req.filepath, out_path, sizeof(req.filepath));
                }

                if (req.playlist_post_id != 0 && req.artwork_post_id != 0) {
                    playlist_update_artwork_status(req.playlist_post_id, req.artwork_post_id, true);
                }

                download_success = true;
                break;
            }

            ESP_LOGW(TAG, "Download attempt %d/%d failed (%s): %s",
                     attempt + 1, max_attempts, esp_err_to_name(err), req.art_url);

            if (attempt + 1 < max_attempts) {
                vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
            }
        }

        // Invoke completion callback on success (and if not cancelled)
        if (download_success && !is_channel_cancelled(req.channel_id)) {
            if (s_completion_cb && req.filepath[0]) {
                s_completion_cb(req.channel_id, req.storage_key, req.filepath, s_completion_ctx);
            }
        }

        set_active_channel(NULL);
        
        // Brief delay between downloads to reduce SDIO bus contention
        // (Wi-Fi and SD card share the SDIO bus)
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t download_manager_init(void)
{
    if (s_queue) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_queue = xQueueCreate(DL_QUEUE_LEN, sizeof(download_request_t));
    if (!s_queue) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(download_task, "download_mgr", 16384, NULL, 5, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Download manager started (queue=%d)", DL_QUEUE_LEN);
    return ESP_OK;
}

void download_manager_deinit(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    s_completion_cb = NULL;
    s_completion_ctx = NULL;
}

esp_err_t download_queue(const download_request_t *req)
{
    if (!req) return ESP_ERR_INVALID_ARG;
    if (!s_queue) return ESP_ERR_INVALID_STATE;

    // Check if this channel is cancelled before queueing
    if (is_channel_cancelled(req->channel_id)) {
        ESP_LOGD(TAG, "Not queueing download for cancelled channel: %s", req->channel_id);
        return ESP_OK;  // Silently skip
    }

    // Best-effort: enqueue, drop if full.
    if (xQueueSend(s_queue, req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Download queue full, dropping: %s", req->storage_key);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t download_queue_artwork(const char *channel_id,
                                 int32_t playlist_post_id,
                                 const artwork_ref_t *artwork,
                                 download_priority_t priority)
{
    if (!artwork) return ESP_ERR_INVALID_ARG;

    download_request_t req = {0};
    req.playlist_post_id = playlist_post_id;
    req.artwork_post_id = artwork->post_id;
    strlcpy(req.storage_key, artwork->storage_key, sizeof(req.storage_key));
    strlcpy(req.art_url, artwork->art_url, sizeof(req.art_url));
    strlcpy(req.filepath, artwork->filepath, sizeof(req.filepath));
    req.priority = priority;
    if (channel_id && channel_id[0]) {
        strlcpy(req.channel_id, channel_id, sizeof(req.channel_id));
    }

    return download_queue(&req);
}

void download_manager_cancel_channel(const char *channel_id)
{
    if (!channel_id || !channel_id[0]) return;
    if (!s_mutex) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        strlcpy(s_cancelled_channel, channel_id, sizeof(s_cancelled_channel));
        ESP_LOGI(TAG, "Cancelling downloads for channel: %s", channel_id);
        
        // Also clear the queue of items for this channel
        // We do this by receiving all items and re-queueing only non-matching ones
        if (s_queue) {
            download_request_t req;
            int cleared = 0;
            int kept = 0;
            
            // Temporarily create a holding queue
            QueueHandle_t temp_queue = xQueueCreate(DL_QUEUE_LEN, sizeof(download_request_t));
            if (temp_queue) {
                // Drain the queue
                while (xQueueReceive(s_queue, &req, 0) == pdTRUE) {
                    if (req.channel_id[0] && strcmp(req.channel_id, channel_id) == 0) {
                        cleared++;
                    } else {
                        // Re-queue to temp
                        xQueueSend(temp_queue, &req, 0);
                        kept++;
                    }
                }
                // Move back from temp to main queue
                while (xQueueReceive(temp_queue, &req, 0) == pdTRUE) {
                    xQueueSend(s_queue, &req, 0);
                }
                vQueueDelete(temp_queue);
                
                if (cleared > 0) {
                    ESP_LOGI(TAG, "Cleared %d queued downloads for channel %s, kept %d", 
                             cleared, channel_id, kept);
                }
            }
        }
        
        xSemaphoreGive(s_mutex);
    }
}

void download_manager_set_completion_callback(download_completion_cb_t cb, void *user_ctx)
{
    s_completion_cb = cb;
    s_completion_ctx = user_ctx;
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

void download_manager_clear_cancelled(const char *channel_id)
{
    if (!s_mutex) return;
    
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // If clearing a specific channel, only clear if it matches
        if (channel_id && channel_id[0]) {
            if (strcmp(s_cancelled_channel, channel_id) == 0) {
                s_cancelled_channel[0] = '\0';
            }
        } else {
            // Clear any cancellation
            s_cancelled_channel[0] = '\0';
        }
        xSemaphoreGive(s_mutex);
    }
}

size_t download_manager_get_pending_count(void)
{
    if (!s_queue) return 0;
    return (size_t)uxQueueMessagesWaiting(s_queue);
}

size_t download_manager_get_queue_space(void)
{
    if (!s_queue) return 0;
    return (size_t)uxQueueSpacesAvailable(s_queue);
}

bool download_manager_wait_for_space(size_t min_space, uint32_t timeout_ms)
{
    if (!s_queue) return false;
    
    uint32_t elapsed_ms = 0;
    const uint32_t poll_interval_ms = 100;
    
    while (true) {
        size_t available = (size_t)uxQueueSpacesAvailable(s_queue);
        if (available >= min_space) {
            return true;
        }
        
        if (timeout_ms > 0 && elapsed_ms >= timeout_ms) {
            return false;
        }
        
        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        elapsed_ms += poll_interval_ms;
    }
}

