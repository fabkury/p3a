// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

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
#include "http_fetch.h"
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

#include "p3a_limits.h"
#define SHOW_URL_MAX_FILE_SIZE      P3A_MAX_ARTWORK_SIZE
#define SHOW_URL_CHUNK_SIZE         (128 * 1024)         // 128 KB (matches makapix_artwork.c)
#define SHOW_URL_TASK_STACK_SIZE    8192
#define SHOW_URL_MAX_URL_LEN        2048
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
            strcasecmp(ext, "png") == 0  ||
            strcasecmp(ext, "apng") == 0);
}

/**
 * @brief Detect image format from magic bytes
 *
 * Pure function: examines the first bytes of a file payload and returns the
 * detected format string. Uses exact byte-signature matching only.
 *
 * @param data Raw file bytes
 * @param len  Length of data in bytes
 * @return One of "PNG", "JPEG", "GIF", "WEBP", or "UNSUPPORTED"
 */
static const char *detect_image_format(const uint8_t *data, size_t len)
{
    // Minimum length guard: longest check (WebP) reads up to offset 11
    if (!data || len < 12) return "UNSUPPORTED";

    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A) {
        return "PNG";
    }

    // JPEG: FF D8 FF
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return "JPEG";
    }

    // GIF: 47 49 46 38 (GIF8) followed by 37 61 (7a) or 39 61 (9a)
    if (data[0] == 0x47 && data[1] == 0x49 && data[2] == 0x46 && data[3] == 0x38) {
        if ((data[4] == 0x37 && data[5] == 0x61) ||
            (data[4] == 0x39 && data[5] == 0x61)) {
            return "GIF";
        }
    }

    // WebP: RIFF (bytes 0-3) + WEBP (bytes 8-11)
    if (data[0] == 0x52 && data[1] == 0x49 && data[2] == 0x46 && data[3] == 0x46 &&
        data[8] == 0x57 && data[9] == 0x45 && data[10] == 0x42 && data[11] == 0x50) {
        return "WEBP";
    }

    return "UNSUPPORTED";
}

/**
 * @brief Map format name to file extension
 *
 * @param format Format string from detect_image_format()
 * @return File extension (without dot), or NULL for "UNSUPPORTED"
 */
static const char *format_to_extension(const char *format)
{
    if (strcmp(format, "PNG") == 0)  return "png";
    if (strcmp(format, "JPEG") == 0) return "jpg";
    if (strcmp(format, "GIF") == 0)  return "gif";
    if (strcmp(format, "WEBP") == 0) return "webp";
    return NULL;
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

// Percent-throttled progress for blocking mode -> on-screen download bar.
typedef struct {
    bool blocking;
    int  last_percent;
} su_progress_t;

static void su_progress_cb(size_t total, size_t content_length, void *ctx)
{
    su_progress_t *p = (su_progress_t *)ctx;
    if (!p->blocking || content_length == 0) return;
    int percent = (int)((total * 100) / content_length);
    if (percent != p->last_percent) {
        p->last_percent = percent;
        p3a_render_set_channel_message("Download", P3A_CHANNEL_MSG_DOWNLOADING, percent, NULL);
    }
}

// Cooperative cancel: a new request or explicit cancel sets s_cancel.
static bool su_should_abort(void *ctx)
{
    (void)ctx;
    return s_cancel;
}

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

        // Pause auto-swap timer so the download progress display isn't
        // interrupted by an artwork swap
        play_scheduler_pause_auto_swap();

        ESP_LOGI(TAG, "Starting download: %s (blocking=%d)", url, (int)blocking);

        // ------------------------------------------------------------------
        // Extract filename and check extension
        // ------------------------------------------------------------------
        char filename[SHOW_URL_MAX_FILENAME_LEN];
        bool needs_format_detection = false;

        if (!extract_filename_from_url(url, filename, sizeof(filename))) {
            // No filename in URL - use fallback base name; format will
            // be detected from file content after download
            strlcpy(filename, "artwork", sizeof(filename));
            needs_format_detection = true;
        } else {
            const char *ext = strrchr(filename, '.');
            if (!ext || !is_supported_extension(ext + 1)) {
                // Extension missing or unrecognized - strip it to keep
                // just the base name; format will be detected later
                needs_format_detection = true;
                if (ext) {
                    // Truncate at the dot to remove unrecognized extension
                    filename[(size_t)(ext - filename)] = '\0';
                }
                // Guard against empty base name after stripping
                if (filename[0] == '\0') {
                    strlcpy(filename, "artwork", sizeof(filename));
                }
            }
        }

        // ------------------------------------------------------------------
        // Check SD card availability
        // ------------------------------------------------------------------
        if (animation_player_is_sd_export_locked()) {
            report_failure(blocking, "SD card shared over USB");
            play_scheduler_resume_auto_swap();
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
        char temp_dir[128];
        if (sd_path_get_animations(animations_dir, sizeof(animations_dir)) != ESP_OK ||
            sd_path_get_temporary(temp_dir, sizeof(temp_dir)) != ESP_OK) {
            report_failure(blocking, "Failed to get SD paths");
            play_scheduler_resume_auto_swap();
            s_busy = false;
            continue;
        }

        // Ensure directories exist
        struct stat st;
        if (stat(temp_dir, &st) != 0) {
            mkdir(temp_dir, 0755);
        }
        if (stat(animations_dir, &st) != 0) {
            mkdir(animations_dir, 0755);
        }

        // ------------------------------------------------------------------
        // Create temp file
        // ------------------------------------------------------------------
        char temp_path[512];
        snprintf(temp_path, sizeof(temp_path), "%s/show_url_%llu.tmp",
                 temp_dir, (unsigned long long)(esp_timer_get_time() / 1000));

        // ------------------------------------------------------------------
        // Show initial progress (blocking mode)
        // ------------------------------------------------------------------
        if (blocking) {
            p3a_render_set_channel_message("Download", P3A_CHANNEL_MSG_DOWNLOADING, 0, NULL);
        }

        // ------------------------------------------------------------------
        // Download to the temp file. The helper handles retry / truncation /
        // size cap and the serialized chunk read-write; leave_temp keeps the
        // file in place so we can sniff its bytes and pick a unique name below.
        // ------------------------------------------------------------------
        su_progress_t prog = { .blocking = blocking, .last_percent = -1 };
        http_fetch_request_t fr = {
            .url = url,
            .timeout_ms = 30000,
            .max_size = SHOW_URL_MAX_FILE_SIZE,
            .chunk_size = SHOW_URL_CHUNK_SIZE,
            .min_size = 12,
            .require_exact_length = true,
            .leave_temp = true,
            .should_abort = su_should_abort,
            .progress = su_progress_cb,
            .user_ctx = &prog,
        };
        http_fetch_result_t res = {0};
        esp_err_t err = http_fetch_to_file(&fr, temp_path, &res);
        if (err != ESP_OK) {
            // The helper already removed the temp file on failure.
            if (err == ESP_ERR_INVALID_STATE) {
                // Cancelled mid-download — silent; just clear the progress bar.
                ESP_LOGI(TAG, "Download cancelled");
                if (blocking) {
                    p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
                }
            } else if (err == ESP_ERR_INVALID_SIZE &&
                       res.content_length > (int64_t)SHOW_URL_MAX_FILE_SIZE) {
                report_failure(blocking, "File exceeds 16 MiB limit");
            } else if (err == ESP_ERR_INVALID_SIZE && res.bytes == 0) {
                report_failure(blocking, "Downloaded file is empty");
            } else if (err == ESP_ERR_INVALID_SIZE) {
                report_failure(blocking, "Incomplete download");
            } else {
                char err_msg[64];
                snprintf(err_msg, sizeof(err_msg), "Download failed (%s)", esp_err_to_name(err));
                report_failure(blocking, err_msg);
            }
            play_scheduler_resume_auto_swap();
            s_busy = false;
            continue;
        }

        size_t total_received = res.bytes;

        // ------------------------------------------------------------------
        // Format detection fallback (when extension was missing/unknown)
        // ------------------------------------------------------------------
        if (needs_format_detection) {
            // Re-open the temp file (the helper already closed it) and read the
            // first 12 bytes to detect the format from magic bytes.
            uint8_t header[12];
            size_t header_read = 0;
            FILE *hf = fopen(temp_path, "rb");
            if (hf) {
                header_read = fread(header, 1, sizeof(header), hf);
                fclose(hf);
            }

            const char *format = detect_image_format(header, header_read);
            const char *detected_ext = format_to_extension(format);

            if (!detected_ext) {
                unlink(temp_path);
                report_failure(blocking, "Unsupported file type");
                play_scheduler_resume_auto_swap();
                s_busy = false;
                continue;
            }

            ESP_LOGI(TAG, "Format auto-detected from file content: %s", format);

            // Build filename with detected extension: base_name.ext
            // Truncate base name if needed to leave room for '.' + ext (max 5 chars)
            size_t max_base = sizeof(filename) - strlen(detected_ext) - 2; // dot + NUL
            if (strlen(filename) > max_base) {
                filename[max_base] = '\0';
            }
            char detected_name[SHOW_URL_MAX_FILENAME_LEN];
            snprintf(detected_name, sizeof(detected_name), "%s.%s", filename, detected_ext);
            strlcpy(filename, detected_name, sizeof(filename));
        }

        // ------------------------------------------------------------------
        // Generate unique filename
        // ------------------------------------------------------------------
        char final_path[512];
        char final_name[SHOW_URL_MAX_FILENAME_LEN];
        if (!generate_unique_filename(animations_dir, filename, final_path, sizeof(final_path),
                                      final_name, sizeof(final_name))) {
            unlink(temp_path);
            report_failure(blocking, "Could not generate unique filename");
            play_scheduler_resume_auto_swap();
            s_busy = false;
            continue;
        }

        ESP_LOGI(TAG, "Target filename: %s", final_name);

        // ------------------------------------------------------------------
        // Move to final path (the helper already flushed + closed the temp)
        // ------------------------------------------------------------------
        if (rename(temp_path, final_path) != 0) {
            ESP_LOGE(TAG, "Failed to rename %s -> %s: %s", temp_path, final_path, strerror(errno));
            unlink(temp_path);
            report_failure(blocking, "Failed to save file");
            play_scheduler_resume_auto_swap();
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

        play_scheduler_resume_auto_swap();
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
