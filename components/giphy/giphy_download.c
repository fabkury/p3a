// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file giphy_download.c
 * @brief Giphy artwork download - HTTP GET to giphy/ folder on SD card
 *
 * Uses the same serialized chunked download pattern as makapix_artwork.c
 * to avoid SDIO bus contention on ESP32-P4.
 */

#include "giphy.h"
#include "giphy_types.h"
#include "config_store.h"
#include "sd_path.h"
#include "sdio_bus.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"
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

// Extension strings
static const char *s_ext_strings[] = { ".webp", ".gif", ".png", ".jpg" };

/**
 * @brief Rendition suffix lookup table
 *
 * Maps (rendition_name, format) to the URL path suffix.
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

esp_err_t giphy_build_download_url(const char *giphy_id, char *out_url, size_t out_len)
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

/**
 * @brief Ensure sharded directory structure exists under giphy base
 */
static esp_err_t ensure_giphy_dirs(const char *giphy_base,
                                    const char *d1, const char *d2, const char *d3)
{
    char path[256];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", giphy_base, d1);
    if (stat(path, &st) != 0 && mkdir(path, 0755) != 0) {
        ESP_LOGE(TAG, "Failed to create dir: %s", path);
        return ESP_FAIL;
    }

    snprintf(path, sizeof(path), "%s/%s/%s", giphy_base, d1, d2);
    if (stat(path, &st) != 0 && mkdir(path, 0755) != 0) {
        ESP_LOGE(TAG, "Failed to create dir: %s", path);
        return ESP_FAIL;
    }

    snprintf(path, sizeof(path), "%s/%s/%s/%s", giphy_base, d1, d2, d3);
    if (stat(path, &st) != 0 && mkdir(path, 0755) != 0) {
        ESP_LOGE(TAG, "Failed to create dir: %s", path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t giphy_download_artwork(const char *giphy_id, uint8_t extension,
                                 char *out_path, size_t out_len)
{
    return giphy_download_artwork_with_progress(giphy_id, extension, out_path, out_len, NULL, NULL);
}

esp_err_t giphy_download_artwork_with_progress(const char *giphy_id, uint8_t extension,
                                               char *out_path, size_t out_len,
                                               giphy_download_progress_cb_t progress_cb,
                                               void *progress_ctx)
{
    if (!giphy_id || !out_path || out_len == 0) {
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

    // Get giphy base path
    char giphy_base[128];
    if (sd_path_get_giphy(giphy_base, sizeof(giphy_base)) != ESP_OK) {
        strlcpy(giphy_base, "/sdcard/p3a/giphy", sizeof(giphy_base));
    }

    // Ensure base directory exists
    struct stat st;
    if (stat(giphy_base, &st) != 0) {
        if (mkdir(giphy_base, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create giphy directory");
            return ESP_FAIL;
        }
    }

    // Compute SHA256 for sharding
    uint8_t sha256[32];
    int ret = mbedtls_sha256((const unsigned char *)giphy_id, strlen(giphy_id), sha256, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "SHA256 failed");
        return ESP_FAIL;
    }

    char d1[3], d2[3], d3[3];
    snprintf(d1, sizeof(d1), "%02x", (unsigned int)sha256[0]);
    snprintf(d2, sizeof(d2), "%02x", (unsigned int)sha256[1]);
    snprintf(d3, sizeof(d3), "%02x", (unsigned int)sha256[2]);

    // Ensure directories exist
    esp_err_t err = ensure_giphy_dirs(giphy_base, d1, d2, d3);
    if (err != ESP_OK) return err;

    // Build final filepath
    int ext_idx = (extension <= 1) ? extension : 0;
    snprintf(out_path, out_len, "%s/%s/%s/%s/%s%s",
             giphy_base, d1, d2, d3, giphy_id, s_ext_strings[ext_idx]);

    // Build download URL
    char url[256];
    err = giphy_build_download_url(giphy_id, url, sizeof(url));
    if (err != ESP_OK) return err;

    // Build temp file path
    char temp_path[264];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", out_path);

    ESP_LOGD(TAG, "Downloading: %s -> %s", url, out_path);

    // Allocate chunk buffer (prefer PSRAM)
    uint8_t *chunk_buffer = heap_caps_malloc(DOWNLOAD_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!chunk_buffer) {
        chunk_buffer = malloc(DOWNLOAD_CHUNK_SIZE);
        if (!chunk_buffer) {
            ESP_LOGE(TAG, "Failed to allocate chunk buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(chunk_buffer);
        return ESP_ERR_NO_MEM;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(chunk_buffer);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    int status = esp_http_client_get_status_code(client);

    if (status == 404) {
        ESP_LOGW(TAG, "Giphy 404: %s", giphy_id);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(chunk_buffer);
        return ESP_ERR_NOT_FOUND;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d for %s", status, giphy_id);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(chunk_buffer);
        return ESP_FAIL;
    }

    // Open temp file for writing
    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open temp file: %s", temp_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(chunk_buffer);
        return ESP_FAIL;
    }

    // Serialized chunked download
    size_t total_written = 0;
    bool download_ok = true;

    while (true) {
        // Read chunk from network
        int chunk_received = 0;
        while (chunk_received < DOWNLOAD_CHUNK_SIZE) {
            int read_len = esp_http_client_read(client,
                                                 (char *)chunk_buffer + chunk_received,
                                                 DOWNLOAD_CHUNK_SIZE - chunk_received);
            if (read_len < 0) {
                ESP_LOGE(TAG, "HTTP read error");
                download_ok = false;
                break;
            }
            if (read_len == 0) break;  // End of data
            chunk_received += read_len;
        }

        if (!download_ok || chunk_received == 0) break;

        // Write chunk to SD card
        size_t written = fwrite(chunk_buffer, 1, chunk_received, f);
        if (written != (size_t)chunk_received) {
            ESP_LOGE(TAG, "Write error: wrote %zu/%d bytes", written, chunk_received);
            download_ok = false;
            break;
        }

        total_written += written;

        if (progress_cb) {
            progress_cb(total_written,
                        (content_length > 0) ? (size_t)content_length : 0,
                        progress_ctx);
        }

        if (chunk_received < DOWNLOAD_CHUNK_SIZE) break;  // Last chunk
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(chunk_buffer);

    if (!download_ok || total_written == 0) {
        unlink(temp_path);
        ESP_LOGE(TAG, "Download failed for %s", giphy_id);
        return ESP_FAIL;
    }

    // Atomic rename
    unlink(out_path);  // Remove old file if exists
    if (rename(temp_path, out_path) != 0) {
        ESP_LOGE(TAG, "Rename failed: %s -> %s", temp_path, out_path);
        unlink(temp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloaded %s (%zu bytes)", giphy_id, total_written);
    return ESP_OK;
}
