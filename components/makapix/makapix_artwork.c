// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "makapix_artwork.h"
#include "sd_path.h"
#include "sdio_bus.h"
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
typedef struct {
    makapix_download_progress_cb progress_cb;
    void *progress_ctx;
    size_t content_length;
    size_t total_received;
    int last_percent;
} download_progress_ctx_t;

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

    // Use temp file for atomic write (power-loss safe)
    char temp_path[path_len + 8];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", out_path);
    
    ESP_LOGD(TAG, "Downloading artwork from %s to %s", full_url, out_path);

    // =========================================================================
    // SERIALIZED CHUNKED DOWNLOAD
    // =========================================================================
    // The ESP32-P4 shares the SDMMC controller between WiFi (SDIO Slot 1) and 
    // SD card (Slot 0). Simultaneous operations cause "SDIO slave unresponsive" 
    // crashes.
    //
    // Solution: Read/write in serialized chunks:
    //   1. Read 1MB chunk from WiFi into RAM (only WiFi/SDIO active)
    //   2. Write chunk to SD card (only SD/SDMMC active)
    //   3. Repeat until complete
    //
    // This uses only 1MB RAM regardless of file size.
    // =========================================================================
    
    // Allocate chunk buffer (prefer PSRAM, fall back to internal)
    uint8_t *chunk_buffer = heap_caps_malloc(DOWNLOAD_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!chunk_buffer) {
        chunk_buffer = malloc(DOWNLOAD_CHUNK_SIZE);
        if (!chunk_buffer) {
            ESP_LOGE(TAG, "Failed to allocate %d KB chunk buffer", DOWNLOAD_CHUNK_SIZE / 1024);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGD(TAG, "Using internal RAM for chunk buffer");
    }
    
    ESP_LOGD(TAG, "Starting chunked download (%d KB chunks)", DOWNLOAD_CHUNK_SIZE / 1024);

    // Configure HTTP client for manual read control
    esp_http_client_config_t config = {
        .url = full_url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,  // Internal receive buffer
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(chunk_buffer);
        return ESP_ERR_NO_MEM;
    }

    // Open connection and fetch headers
    err = esp_http_client_open(client, 0);  // 0 = no write data (GET request)
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(chunk_buffer);
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status %d for URL: %s", status_code, full_url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(chunk_buffer);
        if (status_code == 404) {
            // Permanent miss: do not retry
            return ESP_ERR_NOT_FOUND;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    ESP_LOGD(TAG, "HTTP 200 OK, Content-Length: %lld bytes", content_length);

    // Open temp file for writing
    FILE *fp = fopen(temp_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open temp file: %s (errno=%d)", temp_path, errno);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(chunk_buffer);
        return ESP_FAIL;
    }

    // Progress tracking
    download_progress_ctx_t progress = {
        .progress_cb = cb,
        .progress_ctx = user_ctx,
        .content_length = (content_length > 0) ? (size_t)content_length : 0,
        .total_received = 0,
        .last_percent = -1,
    };

    // =========================================================================
    // MAIN DOWNLOAD LOOP: Read chunk → Write chunk (serialized)
    // =========================================================================
    bool download_error = false;
    
    while (1) {
        // ---------------------------------------------------------------------
        // PHASE A: Read up to 1MB from WiFi into RAM (only WiFi/SDIO active)
        // ---------------------------------------------------------------------
        size_t chunk_received = 0;
        
        while (chunk_received < DOWNLOAD_CHUNK_SIZE) {
            int read_len = esp_http_client_read(
                client, 
                (char *)(chunk_buffer + chunk_received), 
                DOWNLOAD_CHUNK_SIZE - chunk_received
            );
            
            if (read_len < 0) {
                ESP_LOGE(TAG, "HTTP read error: %d", read_len);
                download_error = true;
                break;
            }
            
            if (read_len == 0) {
                // End of stream
                break;
            }
            
            chunk_received += read_len;
        }
        
        if (download_error) {
            break;
        }
        
        // No more data?
        if (chunk_received == 0) {
            break;
        }
        
        // ---------------------------------------------------------------------
        // PHASE B: Write chunk to SD card (only SD/SDMMC active)
        // ---------------------------------------------------------------------
        size_t written = fwrite(chunk_buffer, 1, chunk_received, fp);
        if (written != chunk_received) {
            ESP_LOGE(TAG, "SD write error: wrote %zu of %zu bytes", written, chunk_received);
            download_error = true;
            break;
        }
        
        // Update progress
        progress.total_received += chunk_received;
        
        if (progress.progress_cb && progress.content_length > 0) {
            int percent = (int)((progress.total_received * 100) / progress.content_length);
            if (percent != progress.last_percent) {
                progress.last_percent = percent;
                progress.progress_cb(progress.total_received, progress.content_length, progress.progress_ctx);
            }
        }

        ESP_LOGD(TAG, "Chunk: %zu bytes, Total: %zu / %zu bytes",
                 chunk_received, progress.total_received, progress.content_length);

        // Yield to higher-priority tasks (animation rendering) between chunks
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Cleanup HTTP client
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(chunk_buffer);
    chunk_buffer = NULL;

    // Handle download errors
    if (download_error) {
        ESP_LOGE(TAG, "Download failed after %zu bytes", progress.total_received);
        fclose(fp);
        unlink(temp_path);
        return ESP_FAIL;
    }

    // Validate downloaded size
    if (progress.total_received == 0) {
        ESP_LOGE(TAG, "Downloaded file is empty");
        fclose(fp);
        unlink(temp_path);
        return ESP_ERR_INVALID_SIZE;
    }
    
    if (progress.total_received < 12) {
        ESP_LOGE(TAG, "Downloaded file too small (%zu bytes)", progress.total_received);
        fclose(fp);
        unlink(temp_path);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // File size verification: compare actual bytes written vs expected Content-Length
    // RATIONALE: This serves as our file integrity check, avoiding the CPU cost of
    // hashing the entire file after download. We rely on the HTTP stack (TLS) for
    // preventing corruption during transfer. This check catches truncated downloads
    // and storage write failures.
    // NOTE: Unhealthy/corrupted files that pass size check should be marked for
    // deletion by a separate verification system (not yet implemented).
    if (progress.content_length > 0 && progress.total_received != progress.content_length) {
        ESP_LOGE(TAG, "File size mismatch: received %zu bytes, expected %zu bytes",
                progress.total_received, progress.content_length);
        fclose(fp);
        unlink(temp_path);
        return ESP_ERR_INVALID_SIZE;
    } else if (progress.content_length > 0) {
        ESP_LOGD(TAG, "File size verified: %zu bytes match Content-Length", progress.total_received);
    }

    // Ensure all data is flushed to SD card
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    
    ESP_LOGD(TAG, "Download complete: %zu bytes written to temp file", progress.total_received);

    // =========================================================================
    // ATOMIC RENAME: temp file → final file (power-loss safe)
    // =========================================================================
    if (rename(temp_path, out_path) != 0) {
        ESP_LOGE(TAG, "Failed to rename: %s -> %s (errno=%d)", temp_path, out_path, errno);
        unlink(temp_path);
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Artwork saved successfully (%zu bytes)", progress.total_received);
    return ESP_OK;
}

esp_err_t makapix_artwork_download(const char *art_url, const char *storage_key, char *out_path, size_t path_len)
{
    return makapix_artwork_download_with_progress(art_url, storage_key, out_path, path_len, NULL, NULL);
}

/**
 * @brief File info structure for cache eviction
 */
typedef struct {
    char path[512];  // Increased buffer size
    time_t mtime;
} file_info_t;

/**
 * @brief Compare function for qsort (oldest first)
 */
static int compare_file_info(const void *a, const void *b)
{
    const file_info_t *fa = (const file_info_t *)a;
    const file_info_t *fb = (const file_info_t *)b;
    if (fa->mtime < fb->mtime) return -1;
    if (fa->mtime > fb->mtime) return 1;
    return 0;
}

/**
 * @brief Recursively collect files from vault directory
 */
static size_t collect_files(const char *dir, file_info_t *files, size_t max_files, size_t *count)
{
    DIR *d = opendir(dir);
    if (!d) {
        return *count;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && *count < max_files) {
        if (entry->d_name[0] == '.') {
            continue; // Skip . and ..
        }

        char full_path[512];  // Increased buffer size
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);
        if (ret < 0 || (size_t)ret >= sizeof(full_path)) {
            continue; // Path too long, skip
        }

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // Recursively process subdirectory
                collect_files(full_path, files, max_files, count);
            } else if (S_ISREG(st.st_mode)) {
                // Add file to list
                if (*count < max_files) {
                    snprintf(files[*count].path, sizeof(files[*count].path), "%s", full_path);
                    files[*count].mtime = st.st_mtime;
                    (*count)++;
                }
            }
        }
    }

    closedir(d);
    return *count;
}

esp_err_t makapix_artwork_ensure_cache_limit(size_t max_items)
{
    // Get vault base path
    char vault_base[128];
    if (sd_path_get_vault(vault_base, sizeof(vault_base)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get vault path");
        return ESP_FAIL;
    }

    // Check if vault directory exists
    struct stat st;
    if (stat(vault_base, &st) != 0) {
        // Vault doesn't exist yet, nothing to evict
        return ESP_OK;
    }

    // Allocate buffer for file list (estimate max needed)
    const size_t max_files = max_items * 2; // Allocate extra space
    file_info_t *files = malloc(sizeof(file_info_t) * max_files);
    if (!files) {
        ESP_LOGE(TAG, "Failed to allocate memory for file list");
        return ESP_ERR_NO_MEM;
    }

    // Collect all files
    size_t file_count = 0;
    collect_files(vault_base, files, max_files, &file_count);

    ESP_LOGD(TAG, "Found %zu files in vault", file_count);

    if (file_count <= max_items) {
        free(files);
        return ESP_OK; // No eviction needed
    }

    // Sort by modification time (oldest first)
    qsort(files, file_count, sizeof(file_info_t), compare_file_info);

    // Delete oldest files
    size_t to_delete = file_count - max_items;
    size_t deleted = 0;
    for (size_t i = 0; i < to_delete && i < file_count; i++) {
        if (unlink(files[i].path) == 0) {
            deleted++;
            ESP_LOGD(TAG, "Deleted old file: %s", files[i].path);
        } else {
            ESP_LOGW(TAG, "Failed to delete file: %s", files[i].path);
        }
    }

    ESP_LOGD(TAG, "Evicted %zu files from cache", deleted);

    free(files);
    return ESP_OK;
}

