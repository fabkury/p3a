// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file show_url.c
 * @brief Download artwork from URL and play it
 *
 * Handles the show-url command: downloads an artwork file from an arbitrary
 * HTTP/HTTPS URL to the animations directory, then plays it via the
 * play scheduler.
 *
 * Uses the same serialized chunked download pattern as makapix_artwork.c
 * to avoid SDIO bus conflicts between WiFi and SD card.
 */

#include "show_url.h"
#include "sd_path.h"
#include "p3a_state.h"
#include "p3a_render.h"
#include "play_scheduler.h"
#include "makapix.h"
#include "download_manager.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "show_url";

// ============================================================================
// Constants
// ============================================================================

#define SHOW_URL_MAX_FILE_SIZE      (16 * 1024 * 1024)  // 16 MiB
#define SHOW_URL_CHUNK_SIZE         (128 * 1024)         // 128 KB (matches makapix_artwork.c)
#define SHOW_URL_TASK_STACK_SIZE    6144
#define SHOW_URL_MAX_URL_LEN        512
#define SHOW_URL_MAX_FILENAME_LEN   256

// ============================================================================
// State
// ============================================================================

static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_work_sem = NULL;
static volatile bool s_cancel = false;
static volatile bool s_busy = false;

// Current request (protected by the semaphore -- only written before signal,
// read only by the task after waking)
static char s_artwork_url[SHOW_URL_MAX_URL_LEN];
static bool s_blocking;

// PSRAM-backed stack
static StackType_t *s_task_stack = NULL;
static StaticTask_t s_task_buffer;

// ============================================================================
// External functions
// ============================================================================

extern void proc_notif_fail(void) __attribute__((weak));
extern bool animation_player_is_sd_export_locked(void);

// ============================================================================
// Helpers
// ============================================================================

/**
 * @brief Check if a file extension is a supported image format
 */
static bool is_supported_extension(const char *ext)
{
    if (!ext) return false;
    return (strcasecmp(ext, "gif") == 0  ||
            strcasecmp(ext, "webp") == 0 ||
            strcasecmp(ext, "jpg") == 0  ||
            strcasecmp(ext, "jpeg") == 0 ||
            strcasecmp(ext, "png") == 0);
}

/**
 * @brief Extract filename from URL (last path component)
 *
 * Given "http://example.com/path/to/art.gif?v=2", extracts "art.gif".
 * Strips query string and fragment.
 *
 * @param url Full URL
 * @param out_name Output buffer
 * @param max_len Buffer size
 * @return true if a filename was extracted
 */
static bool extract_filename_from_url(const char *url, char *out_name, size_t max_len)
{
    if (!url || !out_name || max_len == 0) return false;

    // Find the last '/' in the URL
    const char *last_slash = strrchr(url, '/');
    if (!last_slash || *(last_slash + 1) == '\0') return false;

    const char *name_start = last_slash + 1;

    // Find end of filename (before '?' or '#')
    const char *name_end = name_start;
    while (*name_end && *name_end != '?' && *name_end != '#') {
        name_end++;
    }

    size_t name_len = (size_t)(name_end - name_start);
    if (name_len == 0 || name_len >= max_len) return false;

    memcpy(out_name, name_start, name_len);
    out_name[name_len] = '\0';
    return true;
}

/**
 * @brief Generate a unique filename in the animations directory
 *
 * If "art.gif" already exists, tries "art_1.gif", "art_2.gif", etc.
 *
 * @param animations_dir Path to animations directory
 * @param original_name Original filename (e.g. "art.gif")
 * @param out_path Output: full path to the unique file
 * @param out_path_len Size of out_path buffer
 * @param out_name Output: just the unique filename
 * @param out_name_len Size of out_name buffer
 * @return true if a unique name was found
 */
static bool generate_unique_filename(const char *animations_dir,
                                     const char *original_name,
                                     char *out_path, size_t out_path_len,
                                     char *out_name, size_t out_name_len)
{
    struct stat st;

    // Try the original name first
    snprintf(out_path, out_path_len, "%s/%s", animations_dir, original_name);
    if (stat(out_path, &st) != 0) {
        // File doesn't exist - use as-is
        strlcpy(out_name, original_name, out_name_len);
        return true;
    }

    // File exists - find the extension
    const char *dot = strrchr(original_name, '.');
    if (!dot) return false;

    size_t base_len = (size_t)(dot - original_name);
    const char *ext = dot; // includes the dot

    // Try _1, _2, _3, ...
    for (int i = 1; i < 10000; i++) {
        snprintf(out_name, out_name_len, "%.*s_%d%s", (int)base_len, original_name, i, ext);
        snprintf(out_path, out_path_len, "%s/%s", animations_dir, out_name);
        if (stat(out_path, &st) != 0) {
            return true;
        }
    }

    ESP_LOGE(TAG, "Could not find unique filename after 10000 attempts");
    return false;
}

/**
 * @brief Cancel all in-flight download/refresh operations
 */
static void cancel_all_inflight(void)
{
    ESP_LOGI(TAG, "Canceling all in-flight operations");

    // 1. Abort any channel switch in progress
    makapix_abort_channel_load();

    // 2. Stop all Makapix channel refresh tasks
    makapix_cancel_all_refreshes();

    // 3. Stop download manager background downloads
    download_manager_set_channels(NULL, 0);
}

/**
 * @brief Report download failure
 */
static void report_failure(bool blocking, const char *error_msg)
{
    ESP_LOGE(TAG, "Download failed: %s", error_msg);

    if (blocking) {
        // Show error on screen for 3 seconds
        p3a_render_set_channel_message("Download", P3A_CHANNEL_MSG_ERROR, -1, error_msg);
        vTaskDelay(pdMS_TO_TICKS(3000));
        p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
    } else {
        // Show red processing notification for 3 seconds
        if (proc_notif_fail) {
            proc_notif_fail();
        }
    }
}

// ============================================================================
// Download Task
// ============================================================================

static void show_url_task(void *arg)
{
    ESP_LOGI(TAG, "Show-URL task started");

    for (;;) {
        // Sleep until signaled
        xSemaphoreTake(s_work_sem, portMAX_DELAY);

        // Copy request parameters (safe: caller wrote before signaling)
        char url[SHOW_URL_MAX_URL_LEN];
        bool blocking;
        strlcpy(url, s_artwork_url, sizeof(url));
        blocking = s_blocking;
        s_cancel = false;
        s_busy = true;

        ESP_LOGI(TAG, "Starting download: %s (blocking=%d)", url, (int)blocking);

        // ------------------------------------------------------------------
        // Validate URL extension
        // ------------------------------------------------------------------
        char filename[SHOW_URL_MAX_FILENAME_LEN];
        if (!extract_filename_from_url(url, filename, sizeof(filename))) {
            report_failure(blocking, "Could not extract filename from URL");
            s_busy = false;
            continue;
        }

        const char *ext = strrchr(filename, '.');
        if (!ext || !is_supported_extension(ext + 1)) {
            report_failure(blocking, "Unsupported file type");
            s_busy = false;
            continue;
        }

        // ------------------------------------------------------------------
        // Check SD card availability
        // ------------------------------------------------------------------
        if (animation_player_is_sd_export_locked()) {
            report_failure(blocking, "SD card shared over USB");
            s_busy = false;
            continue;
        }

        // ------------------------------------------------------------------
        // Cancel all in-flight operations
        // ------------------------------------------------------------------
        cancel_all_inflight();

        // ------------------------------------------------------------------
        // Resolve paths
        // ------------------------------------------------------------------
        char animations_dir[128];
        char downloads_dir[128];
        if (sd_path_get_animations(animations_dir, sizeof(animations_dir)) != ESP_OK ||
            sd_path_get_downloads(downloads_dir, sizeof(downloads_dir)) != ESP_OK) {
            report_failure(blocking, "Failed to get SD paths");
            s_busy = false;
            continue;
        }

        // Ensure directories exist
        struct stat st;
        if (stat(downloads_dir, &st) != 0) {
            mkdir(downloads_dir, 0755);
        }
        if (stat(animations_dir, &st) != 0) {
            mkdir(animations_dir, 0755);
        }

        // ------------------------------------------------------------------
        // Generate unique filename
        // ------------------------------------------------------------------
        char final_path[512];
        char final_name[SHOW_URL_MAX_FILENAME_LEN];
        if (!generate_unique_filename(animations_dir, filename, final_path, sizeof(final_path),
                                      final_name, sizeof(final_name))) {
            report_failure(blocking, "Could not generate unique filename");
            s_busy = false;
            continue;
        }

        ESP_LOGI(TAG, "Target filename: %s", final_name);

        // ------------------------------------------------------------------
        // Create temp file
        // ------------------------------------------------------------------
        char temp_path[512];
        snprintf(temp_path, sizeof(temp_path), "%s/show_url_%llu.tmp",
                 downloads_dir, (unsigned long long)(esp_timer_get_time() / 1000));

        // ------------------------------------------------------------------
        // Show initial progress (blocking mode)
        // ------------------------------------------------------------------
        if (blocking) {
            p3a_render_set_channel_message("Download", P3A_CHANNEL_MSG_DOWNLOADING, 0, NULL);
        }

        // ------------------------------------------------------------------
        // HTTP client setup
        // ------------------------------------------------------------------
        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 30000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 4096,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            report_failure(blocking, "HTTP client init failed");
            s_busy = false;
            continue;
        }

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            report_failure(blocking, "Connection failed");
            s_busy = false;
            continue;
        }

        int64_t content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);

        if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP %d for %s", status_code, url);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            char err_msg[64];
            snprintf(err_msg, sizeof(err_msg), "HTTP error %d", status_code);
            report_failure(blocking, err_msg);
            s_busy = false;
            continue;
        }

        // Check Content-Length against size limit
        if (content_length > SHOW_URL_MAX_FILE_SIZE) {
            ESP_LOGE(TAG, "File too large: %lld bytes (limit %d)", content_length, SHOW_URL_MAX_FILE_SIZE);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            report_failure(blocking, "File exceeds 16 MiB limit");
            s_busy = false;
            continue;
        }

        // ------------------------------------------------------------------
        // Allocate chunk buffer
        // ------------------------------------------------------------------
        uint8_t *chunk_buffer = heap_caps_malloc(SHOW_URL_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
        if (!chunk_buffer) {
            chunk_buffer = malloc(SHOW_URL_CHUNK_SIZE);
            if (!chunk_buffer) {
                ESP_LOGE(TAG, "Failed to allocate chunk buffer");
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                report_failure(blocking, "Out of memory");
                s_busy = false;
                continue;
            }
        }

        // ------------------------------------------------------------------
        // Open temp file
        // ------------------------------------------------------------------
        FILE *fp = fopen(temp_path, "wb");
        if (!fp) {
            ESP_LOGE(TAG, "Failed to open temp file: %s", strerror(errno));
            free(chunk_buffer);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            report_failure(blocking, "Failed to create temp file");
            s_busy = false;
            continue;
        }

        // ------------------------------------------------------------------
        // Download loop (serialized chunks: read from WiFi, write to SD)
        // ------------------------------------------------------------------
        size_t total_received = 0;
        bool download_error = false;
        bool cancelled = false;
        bool size_exceeded = false;
        int last_percent = -1;

        while (1) {
            // Check cancel flag
            if (s_cancel) {
                ESP_LOGI(TAG, "Download cancelled");
                cancelled = true;
                break;
            }

            // Phase A: Read chunk from network
            size_t chunk_received = 0;
            while (chunk_received < SHOW_URL_CHUNK_SIZE) {
                int read_len = esp_http_client_read(
                    client,
                    (char *)(chunk_buffer + chunk_received),
                    SHOW_URL_CHUNK_SIZE - chunk_received
                );

                if (read_len < 0) {
                    ESP_LOGE(TAG, "HTTP read error: %d", read_len);
                    download_error = true;
                    break;
                }
                if (read_len == 0) {
                    break; // End of stream
                }
                chunk_received += read_len;
            }

            if (download_error) break;
            if (chunk_received == 0) break; // Done

            // Check cumulative size limit (for chunked TE with no Content-Length)
            if (total_received + chunk_received > (size_t)SHOW_URL_MAX_FILE_SIZE) {
                ESP_LOGE(TAG, "File exceeds 16 MiB limit during download");
                size_exceeded = true;
                break;
            }

            // Phase B: Write chunk to SD
            size_t written = fwrite(chunk_buffer, 1, chunk_received, fp);
            if (written != chunk_received) {
                ESP_LOGE(TAG, "SD write error: wrote %zu of %zu", written, chunk_received);
                download_error = true;
                break;
            }

            total_received += chunk_received;

            // Update progress (blocking mode)
            if (blocking && content_length > 0) {
                int percent = (int)((total_received * 100) / (size_t)content_length);
                if (percent != last_percent) {
                    last_percent = percent;
                    p3a_render_set_channel_message("Download", P3A_CHANNEL_MSG_DOWNLOADING, percent, NULL);
                }
            }

            // Yield to animation rendering between chunks
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // ------------------------------------------------------------------
        // Cleanup HTTP and buffer
        // ------------------------------------------------------------------
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(chunk_buffer);
        chunk_buffer = NULL;

        // ------------------------------------------------------------------
        // Handle errors
        // ------------------------------------------------------------------
        if (cancelled || download_error || size_exceeded) {
            fclose(fp);
            unlink(temp_path);

            if (cancelled) {
                // Silently cancelled - clear progress if blocking
                if (blocking) {
                    p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
                }
            } else if (size_exceeded) {
                report_failure(blocking, "File exceeds 16 MiB limit");
            } else {
                report_failure(blocking, "Download failed");
            }

            s_busy = false;
            continue;
        }

        // Validate size
        if (total_received == 0) {
            fclose(fp);
            unlink(temp_path);
            report_failure(blocking, "Downloaded file is empty");
            s_busy = false;
            continue;
        }

        // Size verification (if Content-Length was provided)
        if (content_length > 0 && total_received != (size_t)content_length) {
            ESP_LOGE(TAG, "Size mismatch: received %zu, expected %lld", total_received, content_length);
            fclose(fp);
            unlink(temp_path);
            report_failure(blocking, "Incomplete download");
            s_busy = false;
            continue;
        }

        // ------------------------------------------------------------------
        // Flush and move to final path
        // ------------------------------------------------------------------
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);

        if (rename(temp_path, final_path) != 0) {
            ESP_LOGE(TAG, "Failed to rename %s -> %s: %s", temp_path, final_path, strerror(errno));
            unlink(temp_path);
            report_failure(blocking, "Failed to save file");
            s_busy = false;
            continue;
        }

        ESP_LOGI(TAG, "Download complete: %s (%zu bytes)", final_name, total_received);

        // ------------------------------------------------------------------
        // Refresh SD card cache and play
        // ------------------------------------------------------------------
        play_scheduler_refresh_sdcard_cache();

        // Clear progress message before starting playback
        if (blocking) {
            p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
        }

        esp_err_t play_err = play_scheduler_play_local_file(final_path);
        if (play_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start playback: %s", esp_err_to_name(play_err));
            // File is saved, but playback failed - not a fatal error
        }

        s_busy = false;
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t show_url_init(void)
{
    if (s_task != NULL) {
        ESP_LOGD(TAG, "Already initialized");
        return ESP_OK;
    }

    s_work_sem = xSemaphoreCreateBinary();
    if (!s_work_sem) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return ESP_ERR_NO_MEM;
    }

    // Try PSRAM-backed stack first
    s_task_stack = heap_caps_malloc(SHOW_URL_TASK_STACK_SIZE * sizeof(StackType_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool task_created = false;
    if (s_task_stack) {
        s_task = xTaskCreateStatic(show_url_task, "show_url",
                                   SHOW_URL_TASK_STACK_SIZE, NULL,
                                   tskIDLE_PRIORITY + 2,
                                   s_task_stack, &s_task_buffer);
        task_created = (s_task != NULL);
    }

    if (!task_created) {
        if (xTaskCreate(show_url_task, "show_url",
                        SHOW_URL_TASK_STACK_SIZE, NULL,
                        tskIDLE_PRIORITY + 2, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create task");
            vSemaphoreDelete(s_work_sem);
            s_work_sem = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

esp_err_t show_url_start(const char *artwork_url, bool blocking)
{
    if (!artwork_url || artwork_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_work_sem || !s_task) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Cancel any in-progress download
    if (s_busy) {
        ESP_LOGI(TAG, "Cancelling previous download");
        s_cancel = true;
        // Wait briefly for the task to notice
        for (int i = 0; i < 50 && s_busy; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_busy) {
            ESP_LOGW(TAG, "Previous download did not cancel in time");
        }
    }

    // Store request parameters
    strlcpy(s_artwork_url, artwork_url, sizeof(s_artwork_url));
    s_blocking = blocking;

    // Signal the task
    xSemaphoreGive(s_work_sem);

    return ESP_OK;
}

void show_url_cancel(void)
{
    if (s_busy) {
        s_cancel = true;
    }
}

bool show_url_is_busy(void)
{
    return s_busy;
}
