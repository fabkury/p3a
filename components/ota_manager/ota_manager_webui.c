// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file ota_manager_webui.c
 * @brief Web UI (LittleFS partition) OTA implementation
 */

#include "ota_manager_internal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "ota_webui";

// Web UI OTA max consecutive failures before disabling auto-update
#define WEBUI_OTA_MAX_FAILURES 4

#if CONFIG_OTA_WEBUI_ENABLE

#include "esp_littlefs.h"

// NVS keys for web UI OTA state
#define NVS_WEBUI_PARTITION_INVALID "webui_invalid"
#define NVS_WEBUI_NEEDS_RECOVERY    "webui_recover"
#define NVS_WEBUI_OTA_FAILURES      "webui_failures"

// Web UI OTA state
static struct {
    bool partition_valid;
    bool needs_recovery;
    uint8_t failure_count;
    char current_version[16];
    char available_version[16];
    char available_url[256];
    char available_sha256[65];
    bool update_available;
    SemaphoreHandle_t mutex;
    TaskHandle_t install_task;
    webui_ota_state_t state;
    int progress;
    char status_message[64];
    char error_message[128];
} s_webui_ota = {
    .partition_valid = true,
    .needs_recovery = false,
    .failure_count = 0,
    .update_available = false,
    .state = WEBUI_OTA_STATE_IDLE,
    .progress = 0,
};

esp_err_t webui_ota_init(void)
{
    s_webui_ota.mutex = xSemaphoreCreateMutex();
    if (!s_webui_ota.mutex) {
        ESP_LOGE(TAG, "Failed to create webui mutex");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void webui_ota_deinit(void)
{
    if (s_webui_ota.mutex) {
        vSemaphoreDelete(s_webui_ota.mutex);
        s_webui_ota.mutex = NULL;
    }
}

const char *webui_ota_state_to_string(webui_ota_state_t state)
{
    switch (state) {
        case WEBUI_OTA_STATE_IDLE:        return "idle";
        case WEBUI_OTA_STATE_DOWNLOADING: return "downloading";
        case WEBUI_OTA_STATE_UNMOUNTING:  return "unmounting";
        case WEBUI_OTA_STATE_ERASING:     return "erasing";
        case WEBUI_OTA_STATE_WRITING:     return "writing";
        case WEBUI_OTA_STATE_VERIFYING:   return "verifying";
        case WEBUI_OTA_STATE_REMOUNTING:  return "remounting";
        case WEBUI_OTA_STATE_COMPLETE:    return "complete";
        case WEBUI_OTA_STATE_ERROR:       return "error";
        default:                          return "unknown";
    }
}

static void webui_set_state(webui_ota_state_t new_state, const char *status_message)
{
    if (s_webui_ota.mutex) {
        xSemaphoreTake(s_webui_ota.mutex, portMAX_DELAY);
    }
    s_webui_ota.state = new_state;
    if (status_message) {
        strncpy(s_webui_ota.status_message, status_message, sizeof(s_webui_ota.status_message) - 1);
        s_webui_ota.status_message[sizeof(s_webui_ota.status_message) - 1] = '\0';
    }
    ESP_LOGI(TAG, "WebUI OTA state: %s (%s)", webui_ota_state_to_string(new_state),
             status_message ? status_message : "");
    if (s_webui_ota.mutex) {
        xSemaphoreGive(s_webui_ota.mutex);
    }
}

static void webui_set_progress(int percent, const char *status_message)
{
    if (s_webui_ota.mutex) {
        xSemaphoreTake(s_webui_ota.mutex, portMAX_DELAY);
    }
    s_webui_ota.progress = percent;
    if (status_message) {
        strncpy(s_webui_ota.status_message, status_message, sizeof(s_webui_ota.status_message) - 1);
        s_webui_ota.status_message[sizeof(s_webui_ota.status_message) - 1] = '\0';
    }
    if (s_webui_ota.mutex) {
        xSemaphoreGive(s_webui_ota.mutex);
    }
}

static void webui_set_error(const char *error_message)
{
    if (s_webui_ota.mutex) {
        xSemaphoreTake(s_webui_ota.mutex, portMAX_DELAY);
    }
    s_webui_ota.state = WEBUI_OTA_STATE_ERROR;
    if (error_message) {
        strncpy(s_webui_ota.error_message, error_message, sizeof(s_webui_ota.error_message) - 1);
        s_webui_ota.error_message[sizeof(s_webui_ota.error_message) - 1] = '\0';
        strncpy(s_webui_ota.status_message, error_message, sizeof(s_webui_ota.status_message) - 1);
        s_webui_ota.status_message[sizeof(s_webui_ota.status_message) - 1] = '\0';
    }
    ESP_LOGE(TAG, "WebUI OTA error: %s", error_message ? error_message : "Unknown error");
    if (s_webui_ota.mutex) {
        xSemaphoreGive(s_webui_ota.mutex);
    }
}

static esp_err_t webui_ota_read_nvs_flags(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ota", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        // NVS namespace doesn't exist yet, use defaults
        return ESP_OK;
    }

    uint8_t invalid = 0;
    if (nvs_get_u8(nvs, NVS_WEBUI_PARTITION_INVALID, &invalid) == ESP_OK) {
        s_webui_ota.partition_valid = (invalid == 0);
    }

    uint8_t recover = 0;
    if (nvs_get_u8(nvs, NVS_WEBUI_NEEDS_RECOVERY, &recover) == ESP_OK) {
        s_webui_ota.needs_recovery = (recover != 0);
    }

    uint8_t failures = 0;
    if (nvs_get_u8(nvs, NVS_WEBUI_OTA_FAILURES, &failures) == ESP_OK) {
        s_webui_ota.failure_count = failures;
    }

    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t webui_ota_set_nvs_flag(const char *key, uint8_t value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ota", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

esp_err_t webui_ota_get_current_version(char *version, size_t buf_size)
{
    if (!version || buf_size < 4) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen("/spiffs/version.txt", "r");
    if (!f) {
        // Only log warning if partition is supposed to be valid
        // (avoids noise during OTA updates when filesystem is temporarily unmounted)
        if (s_webui_ota.partition_valid) {
            ESP_LOGW(TAG, "Web UI version.txt not found");
        }
        return ESP_ERR_NOT_FOUND;
    }

    char buf[32] = {0};
    if (fgets(buf, sizeof(buf), f) == NULL) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    fclose(f);

    // Trim whitespace
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' ')) {
        buf[--len] = '\0';
    }

    strncpy(version, buf, buf_size - 1);
    version[buf_size - 1] = '\0';

    return ESP_OK;
}

bool webui_ota_is_partition_healthy(void)
{
    // Read NVS flags
    webui_ota_read_nvs_flags();

    // Check if partition was marked invalid
    if (!s_webui_ota.partition_valid) {
        ESP_LOGW(TAG, "Web UI partition marked invalid in NVS");
        return false;
    }

    // Check if version.txt exists and is readable
    char version[16];
    if (webui_ota_get_current_version(version, sizeof(version)) != ESP_OK) {
        ESP_LOGW(TAG, "Web UI version.txt not readable");
        return false;
    }

    // Store current version
    snprintf(s_webui_ota.current_version, sizeof(s_webui_ota.current_version), "%s", version);

    ESP_LOGI(TAG, "Web UI partition healthy, version: %s", version);
    return true;
}

void webui_ota_set_needs_recovery(void)
{
    s_webui_ota.needs_recovery = true;
    webui_ota_set_nvs_flag(NVS_WEBUI_NEEDS_RECOVERY, 1);
    ESP_LOGW(TAG, "Web UI recovery flagged");
}

esp_err_t webui_ota_get_status(webui_ota_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(webui_ota_status_t));

    // Thread-safe read of state
    if (s_webui_ota.mutex) {
        xSemaphoreTake(s_webui_ota.mutex, portMAX_DELAY);
    }

    // Read current version
    webui_ota_get_current_version(status->current_version, sizeof(status->current_version));

    // Copy cached state
    snprintf(status->available_version, sizeof(status->available_version), "%s", s_webui_ota.available_version);
    status->update_available = s_webui_ota.update_available;
    status->partition_valid = s_webui_ota.partition_valid;
    status->needs_recovery = s_webui_ota.needs_recovery;
    status->failure_count = s_webui_ota.failure_count;
    status->auto_update_disabled = (s_webui_ota.failure_count > WEBUI_OTA_MAX_FAILURES);

    // Copy new state/progress fields
    status->state = s_webui_ota.state;
    status->progress = s_webui_ota.progress;
    snprintf(status->status_message, sizeof(status->status_message), "%s", s_webui_ota.status_message);
    snprintf(status->error_message, sizeof(status->error_message), "%s", s_webui_ota.error_message);

    if (s_webui_ota.mutex) {
        xSemaphoreGive(s_webui_ota.mutex);
    }

    return ESP_OK;
}

// Context for web UI download event handler
typedef struct {
    uint8_t *buffer;
    size_t buffer_size;
    size_t received;
    ota_progress_cb_t progress_cb;
    int content_length;
} webui_download_ctx_t;

static esp_err_t webui_http_event_handler(esp_http_client_event_t *evt)
{
    webui_download_ctx_t *ctx = (webui_download_ctx_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            // Capture content-length for progress reporting
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                ctx->content_length = atoi(evt->header_value);
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (ctx && ctx->buffer && evt->data_len > 0) {
                size_t remaining = ctx->buffer_size - ctx->received;
                size_t to_copy = (evt->data_len < remaining) ? evt->data_len : remaining;
                if (to_copy > 0) {
                    memcpy(ctx->buffer + ctx->received, evt->data, to_copy);
                    ctx->received += to_copy;

                    // Report and persist progress
                    if (ctx->content_length > 0) {
                        int percent = (int)((ctx->received * 100) / ctx->content_length);
                        webui_set_progress(percent, "Downloading web UI...");
                        if (ctx->progress_cb) {
                            ctx->progress_cb(percent, "Downloading web UI...");
                        }
                    }
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t webui_ota_download_and_verify(const char *url,
                                                const char *expected_sha256,
                                                uint8_t **out_data,
                                                size_t *out_size,
                                                ota_progress_cb_t progress_cb)
{
    ESP_LOGI(TAG, "Downloading web UI from: %s", url);

    // Allocate buffer for storage.bin (up to 4MB)
    // Use PSRAM for the download buffer
    size_t max_size = 4 * 1024 * 1024;
    uint8_t *buffer = heap_caps_malloc(max_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate download buffer");
        return ESP_ERR_NO_MEM;
    }

    // Download context for event handler
    webui_download_ctx_t ctx = {
        .buffer = buffer,
        .buffer_size = max_size,
        .received = 0,
        .progress_cb = progress_cb,
        .content_length = 0,
    };

    // Use esp_http_client_perform() which properly handles GitHub redirects to CDN
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CONFIG_OTA_DOWNLOAD_TIMEOUT_SEC * 1000,
        .buffer_size = CONFIG_OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = 1024,
        .max_redirection_count = 5,  // GitHub redirects to CDN
        .event_handler = webui_http_event_handler,
        .user_data = &ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(buffer);
        return err;
    }

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP error: %d", status_code);
        free(buffer);
        return ESP_ERR_HTTP_FETCH_HEADER;
    }

    size_t total_read = ctx.received;
    ESP_LOGI(TAG, "Downloaded %zu bytes", total_read);

    // Verify SHA256
    if (expected_sha256 && strlen(expected_sha256) == 64) {
        if (progress_cb) {
            progress_cb(100, "Verifying checksum...");
        }

        mbedtls_sha256_context sha_ctx;
        uint8_t computed[32];

        mbedtls_sha256_init(&sha_ctx);
        mbedtls_sha256_starts(&sha_ctx, 0);
        mbedtls_sha256_update(&sha_ctx, buffer, total_read);
        mbedtls_sha256_finish(&sha_ctx, computed);
        mbedtls_sha256_free(&sha_ctx);

        // Convert expected to binary
        uint8_t expected[32];
        if (github_ota_hex_to_bin(expected_sha256, expected) != ESP_OK) {
            ESP_LOGE(TAG, "Invalid SHA256 hex string");
            free(buffer);
            return ESP_ERR_INVALID_ARG;
        }

        if (memcmp(computed, expected, 32) != 0) {
            ESP_LOGE(TAG, "SHA256 mismatch!");
            free(buffer);
            return ESP_ERR_INVALID_CRC;
        }

        ESP_LOGI(TAG, "SHA256 verification passed");
    } else {
        ESP_LOGW(TAG, "No SHA256 provided, skipping verification");
    }

    *out_data = buffer;
    *out_size = total_read;
    return ESP_OK;
}

esp_err_t webui_ota_install_update(const char *download_url,
                                    const char *expected_sha256,
                                    ota_progress_cb_t progress_cb)
{
    if (!download_url || strlen(download_url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting web UI OTA update...");

    // Initialize state
    webui_set_state(WEBUI_OTA_STATE_DOWNLOADING, "Starting download...");
    webui_set_progress(0, "Starting download...");
    s_webui_ota.error_message[0] = '\0';

    // Increment failure counter BEFORE starting (defensive)
    s_webui_ota.failure_count++;
    webui_ota_set_nvs_flag(NVS_WEBUI_OTA_FAILURES, s_webui_ota.failure_count);

    // Download and verify
    uint8_t *data = NULL;
    size_t data_size = 0;
    esp_err_t err = webui_ota_download_and_verify(download_url, expected_sha256,
                                                   &data, &data_size, progress_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Download/verify failed: %s", esp_err_to_name(err));
        webui_set_error("Download failed");
        return err;
    }

    // Set partition invalid flag BEFORE modifying partition
    webui_ota_set_nvs_flag(NVS_WEBUI_PARTITION_INVALID, 1);
    s_webui_ota.partition_valid = false;

    webui_set_state(WEBUI_OTA_STATE_UNMOUNTING, "Unmounting filesystem...");
    webui_set_progress(0, "Unmounting filesystem...");
    if (progress_cb) {
        progress_cb(0, "Unmounting filesystem...");
    }

    // Unmount LittleFS
    esp_err_t unmount_err = esp_vfs_littlefs_unregister("storage");
    if (unmount_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to unmount LittleFS: %s (continuing anyway)",
                 esp_err_to_name(unmount_err));
    }

    // Get storage partition
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    if (!partition) {
        ESP_LOGE(TAG, "Storage partition not found");
        free(data);
        webui_set_error("Storage partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Storage partition: offset=0x%lx, size=%lu",
             partition->address, partition->size);

    if (data_size > partition->size) {
        ESP_LOGE(TAG, "Image too large: %zu > %lu", data_size, partition->size);
        free(data);
        webui_set_error("Image too large");
        return ESP_ERR_INVALID_SIZE;
    }

    webui_set_state(WEBUI_OTA_STATE_ERASING, "Erasing partition...");
    webui_set_progress(0, "Erasing partition...");
    if (progress_cb) {
        progress_cb(0, "Erasing partition...");
    }

    // Erase partition
    err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erase failed: %s", esp_err_to_name(err));
        free(data);
        webui_set_error("Partition erase failed");
        return err;
    }

    webui_set_state(WEBUI_OTA_STATE_WRITING, "Writing to flash...");
    webui_set_progress(0, "Writing to flash...");
    if (progress_cb) {
        progress_cb(0, "Writing to flash...");
    }

    // Write data to partition
    size_t offset = 0;
    size_t chunk_size = 4096;
    while (offset < data_size) {
        size_t to_write = (data_size - offset < chunk_size) ? (data_size - offset) : chunk_size;
        err = esp_partition_write(partition, offset, data + offset, to_write);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Write failed at offset %zu: %s", offset, esp_err_to_name(err));
            free(data);
            webui_set_error("Flash write failed");
            return err;
        }
        offset += to_write;

        int percent = (int)((offset * 100) / data_size);
        webui_set_progress(percent, "Writing to flash...");
        if (progress_cb) {
            progress_cb(percent, "Writing to flash...");
        }
    }

    free(data);

    webui_set_state(WEBUI_OTA_STATE_VERIFYING, "Verifying write...");
    webui_set_progress(0, "Verifying write...");
    if (progress_cb) {
        progress_cb(100, "Verifying write...");
    }

    // Post-write verification: read back and verify SHA256
    if (expected_sha256 && strlen(expected_sha256) == 64) {
        mbedtls_sha256_context ctx;
        uint8_t computed[32];
        uint8_t *read_buf = malloc(4096);

        if (!read_buf) {
            ESP_LOGE(TAG, "Failed to allocate verification buffer");
            webui_set_error("Out of memory");
            return ESP_ERR_NO_MEM;
        }

        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);

        offset = 0;
        while (offset < data_size) {
            size_t to_read = (data_size - offset < 4096) ? (data_size - offset) : 4096;
            err = esp_partition_read(partition, offset, read_buf, to_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Verification read failed at offset %zu", offset);
                free(read_buf);
                mbedtls_sha256_free(&ctx);
                webui_set_error("Verification read failed");
                return err;
            }
            mbedtls_sha256_update(&ctx, read_buf, to_read);
            offset += to_read;

            int percent = (int)((offset * 100) / data_size);
            webui_set_progress(percent, "Verifying write...");
        }

        mbedtls_sha256_finish(&ctx, computed);
        mbedtls_sha256_free(&ctx);
        free(read_buf);

        uint8_t expected[32];
        github_ota_hex_to_bin(expected_sha256, expected);

        if (memcmp(computed, expected, 32) != 0) {
            ESP_LOGE(TAG, "Post-write SHA256 verification failed!");
            webui_set_error("Checksum verification failed");
            return ESP_ERR_INVALID_CRC;
        }

        ESP_LOGI(TAG, "Post-write verification passed");
    }

    webui_set_state(WEBUI_OTA_STATE_REMOUNTING, "Remounting filesystem...");
    webui_set_progress(100, "Remounting filesystem...");
    if (progress_cb) {
        progress_cb(100, "Remounting filesystem...");
    }

    // Remount LittleFS
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .format_if_mount_failed = false,
        .dont_mount = false,
    };

    err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to remount LittleFS: %s", esp_err_to_name(err));
        webui_set_error("Failed to remount filesystem");
        return err;
    }

    // Verify version.txt is readable
    char new_version[16] = {0};
    err = webui_ota_get_current_version(new_version, sizeof(new_version));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read version.txt after update");
        webui_set_error("Failed to verify update");
        return err;
    }

    ESP_LOGI(TAG, "Web UI updated successfully to version %s", new_version);

    // Clear all failure flags - update succeeded!
    webui_ota_set_nvs_flag(NVS_WEBUI_PARTITION_INVALID, 0);
    webui_ota_set_nvs_flag(NVS_WEBUI_NEEDS_RECOVERY, 0);
    webui_ota_set_nvs_flag(NVS_WEBUI_OTA_FAILURES, 0);

    s_webui_ota.partition_valid = true;
    s_webui_ota.needs_recovery = false;
    s_webui_ota.failure_count = 0;
    s_webui_ota.update_available = false;
    snprintf(s_webui_ota.current_version, sizeof(s_webui_ota.current_version), "%s", new_version);

    webui_set_state(WEBUI_OTA_STATE_COMPLETE, "Update complete!");
    webui_set_progress(100, "Update complete!");
    if (progress_cb) {
        progress_cb(100, "Web UI update complete!");
    }

    // After a short delay, return to idle
    vTaskDelay(pdMS_TO_TICKS(3000));
    webui_set_state(WEBUI_OTA_STATE_IDLE, "");
    s_webui_ota.progress = 0;
    s_webui_ota.status_message[0] = '\0';

    return ESP_OK;
}

static void webui_ota_repair_task(void *arg)
{
    ESP_LOGI(TAG, "Web UI repair task started");

    // Get the latest manifest to find the download URL
    github_release_manifest_t manifest;
    esp_err_t err = github_ota_get_release_manifest(&manifest);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get release manifest for repair");
        s_webui_ota.install_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (strlen(manifest.webui.download_url) == 0) {
        ESP_LOGE(TAG, "No web UI download URL in manifest");
        s_webui_ota.install_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Install the update
    err = webui_ota_install_update(manifest.webui.download_url,
                                    manifest.webui.sha256, NULL);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web UI repair failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Web UI repair completed successfully");
    }

    s_webui_ota.install_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t webui_ota_trigger_repair(void)
{
    if (s_webui_ota.install_task != NULL) {
        ESP_LOGW(TAG, "Web UI repair already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    // Bypass failure counter for manual repairs
    BaseType_t ret = xTaskCreate(webui_ota_repair_task, "webui_repair",
                                  8192, NULL, 5, &s_webui_ota.install_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create repair task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

#else // !CONFIG_OTA_WEBUI_ENABLE

// Stub implementations when web UI OTA is disabled

esp_err_t webui_ota_init(void)
{
    return ESP_OK;
}

void webui_ota_deinit(void)
{
}

const char *webui_ota_state_to_string(webui_ota_state_t state)
{
    (void)state;
    return "disabled";
}

esp_err_t webui_ota_get_current_version(char *version, size_t buf_size)
{
    if (version && buf_size > 0) {
        version[0] = '\0';
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t webui_ota_get_status(webui_ota_status_t *status)
{
    if (status) {
        memset(status, 0, sizeof(webui_ota_status_t));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

bool webui_ota_is_partition_healthy(void)
{
    return true;  // Assume healthy when OTA is disabled
}

void webui_ota_set_needs_recovery(void)
{
    // No-op
}

esp_err_t webui_ota_trigger_repair(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t webui_ota_install_update(const char *download_url,
                                    const char *expected_sha256,
                                    ota_progress_cb_t progress_cb)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif // CONFIG_OTA_WEBUI_ENABLE
