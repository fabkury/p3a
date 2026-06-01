// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_artwork.c
 * @brief Makapix artwork HTTP download to vault with SHA256-sharded storage
 */

#include "makapix_artwork.h"
#include "http_fetch.h"
#include "p3a_limits.h"
#include "sd_path.h"
#include "sdio_bus.h"
#include "makapix_channel_events.h"
#include "download_manager.h"  // download_manager_is_canceled (S1 cooperative cancel)
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>

static const char *TAG = "makapix_artwork";

// Chunk size for serialized download (read chunk from WiFi, then write to SD)
// 64 KB provides good balance between throughput, memory usage and animation jitter.
#define DOWNLOAD_CHUNK_SIZE (32 * 1024)

// Extension strings for file naming
static const char *s_ext_strings[] = { ".webp", ".gif", ".png", ".jpg" };

/**
 * @brief Detect file extension from URL
 * @return Extension index (0=webp, 1=gif, 2=png, 3=jpg), defaults to webp
 */
static int detect_extension_from_url(const char *url)
{
    if (!url) return 0;
    size_t len = strlen(url);
    // Check longer extensions first (e.g., .jpeg before .jpg)
    if (len >= 5 && strcasecmp(url + len - 5, ".webp") == 0) return 0;
    if (len >= 5 && strcasecmp(url + len - 5, ".jpeg") == 0) return 3; // JPEG (prefer .jpg but accept .jpeg)
    if (len >= 4 && strcasecmp(url + len - 4, ".gif") == 0)  return 1;
    if (len >= 4 && strcasecmp(url + len - 4, ".png") == 0)  return 2;
    if (len >= 4 && strcasecmp(url + len - 4, ".jpg") == 0)  return 3; // JPEG (canonical extension)
    return 0; // Default to webp
}

/**
 * @brief SHA256(storage_key) helper (used for vault sharding and URL syntax)
 */
static esp_err_t storage_key_sha256(const char *storage_key, uint8_t out_sha256[32])
{
    if (!storage_key || !out_sha256) return ESP_ERR_INVALID_ARG;
    // mbedTLS: is224=0 => SHA-256
    int ret = mbedtls_sha256((const unsigned char *)storage_key, strlen(storage_key), out_sha256, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "SHA256 failed (ret=%d)", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief Ensure vault directory structure exists
 */
static esp_err_t ensure_vault_dirs(const char *vault_base, const char *dir1, const char *dir2, const char *dir3)
{
    char path[256];
    struct stat st;
    
    // Create first level directory
    snprintf(path, sizeof(path), "%s/%s", vault_base, dir1);
    if (stat(path, &st) != 0) {
        if (mkdir(path, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create directory %s", path);
            return ESP_FAIL;
        }
    }

    // Create second level directory
    snprintf(path, sizeof(path), "%s/%s/%s", vault_base, dir1, dir2);
    if (stat(path, &st) != 0) {
        if (mkdir(path, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create directory %s", path);
            return ESP_FAIL;
        }
    }

    // Create third level directory
    snprintf(path, sizeof(path), "%s/%s/%s/%s", vault_base, dir1, dir2, dir3);
    if (stat(path, &st) != 0) {
        if (mkdir(path, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create directory %s", path);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

/**
 * @brief Serialized chunked download context
 * 
 * IMPORTANT: SDMMC Bus Contention Avoidance
 * =========================================
 * The ESP32-P4 shares the SDMMC controller between WiFi (SDIO Slot 1) and 
 * SD card (Slot 0). Simultaneous operations cause "SDIO slave unresponsive" crashes.
 * 
 * Solution: Serialized chunked download
 * - Read a chunk from WiFi into RAM (only WiFi active)
 * - Write the chunk to SD card (only SD active)
 * - Repeat until complete
 * 
 * This uses only 1MB of RAM regardless of file size, while keeping operations serialized.
 */
// Percent-throttled progress: the helper invokes us every chunk; we forward to
// the caller's callback only when the integer percent changes (preserves the
// pre-refactor behavior).
typedef struct {
    makapix_download_progress_cb cb;
    void *ctx;
    int last_percent;
} mk_progress_t;

static void mk_progress_cb(size_t total, size_t content_length, void *ctx)
{
    mk_progress_t *p = (mk_progress_t *)ctx;
    if (!p->cb || content_length == 0) return;
    int percent = (int)((total * 100) / content_length);
    if (percent != p->last_percent) {
        p->last_percent = percent;
        p->cb(total, content_length, p->ctx);
    }
}

// Cooperative abort: SD card exported to USB, or playset switched mid-download.
static bool mk_should_abort(void *ctx)
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

esp_err_t makapix_artwork_download_with_progress(const char *art_url, const char *storage_key,
                                                 char *out_path, size_t path_len,
                                                 makapix_download_progress_cb cb, void *user_ctx)
{
    if (!art_url || !storage_key || !out_path || path_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Wait if SDIO bus is locked (e.g., by OTA check/download)
    // This prevents artwork downloads from conflicting with critical WiFi operations
    if (sdio_bus_is_locked()) {
        const char *holder = sdio_bus_get_holder();
        ESP_LOGI(TAG, "SDIO bus locked by %s, waiting before download...", holder ? holder : "unknown");

        int wait_count = 0;
        const int max_wait = 120;  // Wait up to 120 seconds (OTA can take time)
        while (sdio_bus_is_locked() && wait_count < max_wait) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            wait_count++;
        }

        if (wait_count >= max_wait) {
            ESP_LOGE(TAG, "SDIO bus still locked after %d seconds, aborting download",
                     max_wait);
            return ESP_ERR_TIMEOUT;  // Return error instead of proceeding
        }
        ESP_LOGI(TAG, "SDIO bus available after %d seconds", wait_count);
    }

    // Get vault base path
    char vault_base[128];
    if (sd_path_get_vault(vault_base, sizeof(vault_base)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get vault path");
        return ESP_FAIL;
    }

    // Ensure vault base directory exists
    struct stat st;
    if (stat(vault_base, &st) != 0) {
        if (mkdir(vault_base, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create vault directory");
            return ESP_FAIL;
        }
    }

    // Derive folder structure from SHA256(storage_key): /vault/aa/bb/cc/<storage_key>.<ext>
    uint8_t sha256[32];
    esp_err_t err = storage_key_sha256(storage_key, sha256);
    if (err != ESP_OK) return err;
    char dir1[3], dir2[3], dir3[3];
    snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)sha256[0]);
    snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)sha256[1]);
    snprintf(dir3, sizeof(dir3), "%02x", (unsigned int)sha256[2]);

    // Ensure directories exist
    err = ensure_vault_dirs(vault_base, dir1, dir2, dir3);
    if (err != ESP_OK) {
        return err;
    }

    // Detect extension from URL and build file path WITH extension
    int ext_idx = detect_extension_from_url(art_url);
    snprintf(out_path, path_len, "%s/%s/%s/%s/%s%s", vault_base, dir1, dir2, dir3, storage_key, s_ext_strings[ext_idx]);

    // Build full URL - if art_url starts with '/', prepend https://hostname
    char full_url[512];
    if (art_url[0] == '/') {
        // Relative URL - prepend hostname with HTTPS
        snprintf(full_url, sizeof(full_url), "https://%s%s", CONFIG_MAKAPIX_CLUB_HOST, art_url);
    } else {
        // Already a full URL - use as-is
        size_t copy_len = strlen(art_url);
        if (copy_len >= sizeof(full_url)) {
            copy_len = sizeof(full_url) - 1;
        }
        memcpy(full_url, art_url, copy_len);
        full_url[copy_len] = '\0';
    }

    ESP_LOGD(TAG, "Downloading artwork from %s to %s", full_url, out_path);

    // The ESP32-P4 shares the SDMMC controller between WiFi (SDIO) and SD card,
    // so the helper reads a chunk from WiFi then writes it to SD, serialized,
    // never running both at once. Exact Content-Length match is our integrity
    // check (avoids hashing the whole file); a mismatch retries then fails.
    mk_progress_t prog = { .cb = cb, .ctx = user_ctx, .last_percent = -1 };
    http_fetch_request_t fr = {
        .url = full_url,
        .timeout_ms = 30000,
        .max_size = P3A_MAX_ARTWORK_SIZE,
        .chunk_size = DOWNLOAD_CHUNK_SIZE,
        .min_size = 12,
        .require_exact_length = true,
        .should_abort = mk_should_abort,
        .progress = mk_progress_cb,
        .user_ctx = &prog,
    };
    err = http_fetch_to_file(&fr, out_path, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Artwork download failed for %s: %s", storage_key, esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG, "Artwork saved successfully -> %s", out_path);
    return ESP_OK;
}

esp_err_t makapix_artwork_download(const char *art_url, const char *storage_key, char *out_path, size_t path_len)
{
    return makapix_artwork_download_with_progress(art_url, storage_key, out_path, path_len, NULL, NULL);
}
