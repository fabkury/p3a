// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file giphy_download.c
 * @brief Giphy artwork download - HTTP GET to giphy/ folder on SD card
 *
 * Uses the same serialized chunked download pattern as makapix_artwork.c
 * to avoid SDIO bus contention on ESP32-P4.
 */

#include "giphy.h"
#include "giphy_types.h"
#include "p3a_limits.h"
#include "config_store.h"
#include "sd_path.h"
#include "sdio_bus.h"
#include "makapix_channel_events.h"
#include "download_manager.h"  // download_manager_is_canceled (S1 cooperative cancel)
#include "http_fetch.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "giphy_dl";

// Chunk size for serialized download (matches makapix_artwork.c)
#define DOWNLOAD_CHUNK_SIZE (32 * 1024)


/**
 * @brief Rendition suffix lookup table
 *
 * Maps (rendition_name, format) to the URL path suffix.
 *
 * Caveat: only the fixed_height/fixed_width/original files are guaranteed to
 * exist on the CDN. The downsized* rows are best-effort — Giphy materializes
 * those derivatives for only some GIFs (for the rest the API's rendition url
 * points at giphy.gif, and the guessed filename 404s). That is why the
 * per-entry override path (giphy_build_download_url_for_entry) stores which
 * file the API actually referenced instead of trusting this table.
 */
typedef struct {
    const char *rendition;
    const char *format;
    const char *suffix;
} rendition_map_t;

static const rendition_map_t s_rendition_map[] = {
    { "fixed_height",     "webp", "200.webp" },
    { "fixed_height",     "gif",  "200.gif" },
    { "fixed_width",      "webp", "200w.webp" },
    { "fixed_width",      "gif",  "200w.gif" },
    { "original",         "webp", "giphy.webp" },
    { "original",         "gif",  "giphy.gif" },
    { "downsized_medium", "gif",  "giphy-downsized-medium.gif" },
    { "downsized",        "gif",  "giphy-downsized.gif" },
    { NULL, NULL, NULL }
};

/**
 * @brief Build download URL from giphy_id + configured rendition/format
 *
 * Entry-unaware: ignores the per-entry downsized_medium override, so it must
 * only be used as the fallback inside giphy_build_download_url_for_entry().
 */
static esp_err_t giphy_build_download_url(const char *giphy_id, char *out_url, size_t out_len)
{
    if (!giphy_id || !out_url || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Get configured rendition and format
    char rendition[32];
    if (config_store_get_giphy_rendition(rendition, sizeof(rendition)) != ESP_OK) {
        strlcpy(rendition, CONFIG_GIPHY_RENDITION_DEFAULT, sizeof(rendition));
    }

    char format[8];
    if (config_store_get_giphy_format(format, sizeof(format)) != ESP_OK) {
        strlcpy(format, CONFIG_GIPHY_FORMAT_DEFAULT, sizeof(format));
    }

    // Look up suffix
    const char *suffix = NULL;
    for (const rendition_map_t *m = s_rendition_map; m->rendition != NULL; m++) {
        if (strcmp(m->rendition, rendition) == 0 && strcmp(m->format, format) == 0) {
            suffix = m->suffix;
            break;
        }
    }

    if (!suffix) {
        // Fallback: fixed_height gif
        ESP_LOGW(TAG, "Unknown rendition/format combo: %s/%s, falling back to fixed_height/gif",
                 rendition, format);
        suffix = "200.gif";
    }

    snprintf(out_url, out_len, "https://i.giphy.com/media/%s/%s", giphy_id, suffix);
    return ESP_OK;
}

esp_err_t giphy_build_download_url_for_entry(const giphy_channel_entry_t *entry,
                                              char *out_url, size_t out_len)
{
    if (!entry || !out_url || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (entry->reserved[0] == 1) {
        // downsized_medium override — dedicated derivative file (always GIF)
        snprintf(out_url, out_len, "https://i.giphy.com/media/%s/giphy-downsized-medium.gif",
                 entry->giphy_id);
        ESP_LOGD(TAG, "Download URL: downsized_medium for %s", entry->giphy_id);
        return ESP_OK;
    }

    if (entry->reserved[0] == 2) {
        // downsized_medium override — passthrough: the API's rendition url
        // pointed at giphy.gif (no dedicated downsized-medium file exists on
        // the CDN; requesting one would 404)
        snprintf(out_url, out_len, "https://i.giphy.com/media/%s/giphy.gif",
                 entry->giphy_id);
        ESP_LOGD(TAG, "Download URL: downsized_medium via giphy.gif for %s", entry->giphy_id);
        return ESP_OK;
    }

    // Default: use configured rendition/format
    ESP_LOGD(TAG, "Download URL: configured rendition for %s", entry->giphy_id);
    return giphy_build_download_url(entry->giphy_id, out_url, out_len);
}

esp_err_t giphy_download_artwork(const char *giphy_id, const char *url, uint8_t extension,
                                 char *out_path, size_t out_len)
{
    return giphy_download_artwork_with_progress(giphy_id, url, extension, out_path, out_len, NULL, NULL);
}

// Cooperative abort: stop mid-download if the SD card got exported to USB or the
// playset switched (download_manager cancel). Mirrors the per-chunk inline checks
// the old read loop performed.
static bool giphy_dl_should_abort(void *ctx)
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

esp_err_t giphy_download_artwork_with_progress(const char *giphy_id, const char *url,
                                               uint8_t extension,
                                               char *out_path, size_t out_len,
                                               giphy_download_progress_cb_t progress_cb,
                                               void *progress_ctx)
{
    if (!giphy_id || !url || !url[0] || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Wait if SDIO bus is locked
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

    // Build the sharded target path (shared with the reader so writer/reader
    // can't disagree) and create its parent directories.
    if (giphy_build_filepath(giphy_id, extension, out_path, out_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build giphy path for %s", giphy_id);
        return ESP_FAIL;
    }
    esp_err_t err = sd_path_ensure_parent_dirs(out_path);
    if (err != ESP_OK) return err;

    ESP_LOGD(TAG, "Downloading: %s -> %s", url, out_path);

    http_fetch_request_t fr = {
        .url = url,
        .max_size = P3A_MAX_ARTWORK_SIZE,
        .chunk_size = DOWNLOAD_CHUNK_SIZE,
        .treat_empty_as_not_found = true,
        .progress = progress_cb,
        .should_abort = giphy_dl_should_abort,
        .user_ctx = progress_ctx,
    };
    err = http_fetch_to_file(&fr, out_path, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Download failed for %s: %s", giphy_id, esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG, "Downloaded %s", giphy_id);
    return ESP_OK;
}
