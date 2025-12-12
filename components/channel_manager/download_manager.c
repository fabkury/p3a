#include "download_manager.h"
#include "makapix_artwork.h"
#include "sdio_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "dl_mgr";

extern bool animation_player_is_sd_paused(void) __attribute__((weak));

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;

#define DL_QUEUE_LEN 24

static bool file_exists(const char *path)
{
    if (!path || !*path) return false;
    struct stat st;
    return (stat(path, &st) == 0);
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

        // If already downloaded, just update playlist availability if applicable.
        if (file_exists(req.filepath)) {
            if (req.playlist_post_id != 0 && req.artwork_post_id != 0) {
                playlist_update_artwork_status(req.playlist_post_id, req.artwork_post_id, true);
            }
            continue;
        }

        // Only Makapix-style downloads for now (needs URL + storage_key).
        if (req.storage_key[0] == '\0' || req.art_url[0] == '\0') {
            ESP_LOGW(TAG, "Skipping download (missing storage_key/art_url)");
            continue;
        }

        // Retry with backoff: 1s, 5s, 15s
        const int backoff_ms[] = { 1000, 5000, 15000 };
        const int max_attempts = 3;

        for (int attempt = 0; attempt < max_attempts; attempt++) {
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
                break;
            }

            ESP_LOGW(TAG, "Download attempt %d/%d failed (%s): %s",
                     attempt + 1, max_attempts, esp_err_to_name(err), req.art_url);

            if (attempt + 1 < max_attempts) {
                vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
            }
        }
    }
}

esp_err_t download_manager_init(void)
{
    if (s_queue) return ESP_OK;

    s_queue = xQueueCreate(DL_QUEUE_LEN, sizeof(download_request_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(download_task, "download_mgr", 16384, NULL, 5, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Download manager started");
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
}

esp_err_t download_queue(const download_request_t *req)
{
    if (!req) return ESP_ERR_INVALID_ARG;
    if (!s_queue) return ESP_ERR_INVALID_STATE;

    // Best-effort: enqueue, drop if full.
    if (xQueueSend(s_queue, req, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t download_queue_artwork(int32_t playlist_post_id, const artwork_ref_t *artwork, download_priority_t priority)
{
    if (!artwork) return ESP_ERR_INVALID_ARG;

    download_request_t req = {0};
    req.playlist_post_id = playlist_post_id;
    req.artwork_post_id = artwork->post_id;
    strlcpy(req.storage_key, artwork->storage_key, sizeof(req.storage_key));
    strlcpy(req.art_url, artwork->art_url, sizeof(req.art_url));
    strlcpy(req.filepath, artwork->filepath, sizeof(req.filepath));
    req.priority = priority;

    return download_queue(&req);
}

