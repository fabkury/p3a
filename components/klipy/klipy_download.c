// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file klipy_download.c
 * @brief Klipy artwork download - re-resolves the opaque CDN url, then GETs it
 *
 * Klipy CDN urls are per-format random tokens that cannot be reconstructed from
 * the item id, so unlike Giphy we cannot build the url from the stored entry.
 * Instead we re-resolve GET {product}/{id} at download time to obtain the url,
 * then stream it to the SD card with the same serialized chunked pattern Giphy
 * uses. The id->url resolve is one small request per artwork's first download;
 * the file is then cached on SD forever.
 */

#include "klipy.h"
#include "klipy_types.h"
#include "klipy_internal.h"
#include "p3a_limits.h"
#include "p3a_board.h"
#include "config_store.h"
#include "sd_path.h"
#include "sdio_bus.h"
#include "makapix_channel_events.h"
#include "download_manager.h"  // download_manager_is_canceled
#include "http_fetch.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "klipy_dl";

#define DOWNLOAD_CHUNK_SIZE   (32 * 1024)
#define KLIPY_RESOLVE_BUF_SIZE (32 * 1024)

esp_err_t klipy_download_artwork(uint64_t klipy_id, uint8_t product, uint8_t extension,
                                 char *out_path, size_t out_len)
{
    return klipy_download_artwork_with_progress(klipy_id, product, extension,
                                                out_path, out_len, NULL, NULL);
}

static bool klipy_dl_should_abort(void *ctx)
{
    (void)ctx;
    if (!makapix_channel_is_sd_available()) {
        ESP_LOGI(TAG, "Aborting download: SD card exported to USB host");
        return true;
    }
    if (download_manager_is_canceled()) {
        ESP_LOGI(TAG, "Aborting download: playset switched");
        return true;
    }
    return false;
}

/**
 * @brief Resolve the CDN url for an artwork by re-fetching GET {product}/{id}
 *
 * @param extension  0=webp, 1=gif (preferred format to resolve)
 * @param out_url    receives the resolved absolute CDN url
 */
static esp_err_t klipy_resolve_url(uint64_t klipy_id, uint8_t product, uint8_t extension,
                                   char *out_url, size_t out_url_len)
{
    char api_key[128];
    config_store_get_klipy_api_key(api_key, sizeof(api_key));
    if (api_key[0] == '\0') return ESP_ERR_NOT_FOUND;

    char url[256];
    snprintf(url, sizeof(url), "https://api.klipy.com/api/v1/%s/%s/%llu",
             api_key, product == KLIPY_PRODUCT_STICKER ? "stickers" : "gifs",
             (unsigned long long)klipy_id);

    char *buf = malloc(KLIPY_RESOLVE_BUF_SIZE);
    if (!buf) return ESP_ERR_NO_MEM;

    http_fetch_request_t fr = { .url = url, .timeout_ms = 15000 };
    http_fetch_result_t res = {0};
    esp_err_t err = http_fetch_to_buffer(&fr, buf, KLIPY_RESOLVE_BUF_SIZE, NULL, &res);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "resolve failed for %llu: %s (HTTP %d)",
                 (unsigned long long)klipy_id, esp_err_to_name(err), res.http_status);
        free(buf);
        return err;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_FAIL;

    // Single-item resolve returns {result, data:{...item...}}. Be tolerant of a
    // data.data wrapper too.
    const cJSON *data = cJSON_GetObjectItem(root, "data");
    const cJSON *item = data;
    if (cJSON_IsObject(data)) {
        const cJSON *inner = cJSON_GetObjectItem(data, "data");
        if (cJSON_IsObject(inner)) {
            item = inner;
        } else if (cJSON_IsArray(inner) && cJSON_GetArraySize(inner) > 0) {
            item = cJSON_GetArrayItem(inner, 0);
        }
    }

    const cJSON *file = cJSON_GetObjectItem(item, "file");
    const char *picked = NULL;
    uint16_t w = 0, h = 0;
    bool used_gif = false;
    bool ok = klipy_pick_rendition(file, extension == 1 ? "gif" : "webp",
                                   P3A_DISPLAY_WIDTH, P3A_DISPLAY_HEIGHT,
                                   &picked, &w, &h, &used_gif);
    if (ok && picked) {
        strlcpy(out_url, picked, out_url_len);
    }
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t klipy_download_artwork_with_progress(uint64_t klipy_id, uint8_t product, uint8_t extension,
                                               char *out_path, size_t out_len,
                                               klipy_download_progress_cb_t progress_cb,
                                               void *progress_ctx)
{
    if (!out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Wait if SDIO bus is locked (mirrors giphy_download_artwork)
    if (sdio_bus_is_locked()) {
        const char *holder = sdio_bus_get_holder();
        ESP_LOGI(TAG, "SDIO bus locked by %s, waiting...", holder ? holder : "unknown");
        int wait_count = 0;
        while (sdio_bus_is_locked() && wait_count < 120) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            wait_count++;
        }
        if (wait_count >= 120) {
            ESP_LOGE(TAG, "SDIO bus still locked after 120s, aborting");
            return ESP_ERR_TIMEOUT;
        }
    }

    // Re-resolve the opaque CDN url for this artwork.
    char url[320];
    esp_err_t err = klipy_resolve_url(klipy_id, product, extension, url, sizeof(url));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot resolve Klipy url for %llu: %s",
                 (unsigned long long)klipy_id, esp_err_to_name(err));
        return err;
    }

    // Build the sharded target path (shared with the reader) and create dirs.
    if (klipy_build_filepath(klipy_id, product, extension, out_path, out_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build klipy path for %llu", (unsigned long long)klipy_id);
        return ESP_FAIL;
    }
    err = sd_path_ensure_parent_dirs(out_path);
    if (err != ESP_OK) return err;

    ESP_LOGD(TAG, "Downloading %llu -> %s", (unsigned long long)klipy_id, out_path);

    http_fetch_request_t fr = {
        .url = url,
        .max_size = P3A_MAX_ARTWORK_SIZE,
        .chunk_size = DOWNLOAD_CHUNK_SIZE,
        .treat_empty_as_not_found = true,
        .progress = progress_cb,
        .should_abort = klipy_dl_should_abort,
        .user_ctx = progress_ctx,
    };
    err = http_fetch_to_file(&fr, out_path, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Download failed for %llu: %s",
                 (unsigned long long)klipy_id, esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG, "Downloaded %llu", (unsigned long long)klipy_id);
    return ESP_OK;
}
