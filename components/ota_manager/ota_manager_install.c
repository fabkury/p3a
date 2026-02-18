// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file ota_manager_install.c
 * @brief Firmware install, rollback, and boot validation
 */

#include "ota_manager_internal.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "p3a_state.h"
#include "p3a_render.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "ota_install";

static esp_err_t ota_verify_partition_sha256(const esp_partition_t *partition,
                                              size_t size,
                                              const uint8_t expected[32])
{
    if (!partition || !expected || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Verifying SHA256 of partition %s (%zu bytes)...", partition->label, size);

    mbedtls_sha256_context ctx;
    uint8_t computed[32];
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

    if (!buf) {
        buf = malloc(4096);
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }
    }

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    size_t offset = 0;
    while (offset < size) {
        size_t chunk = (size - offset < 4096) ? (size - offset) : 4096;
        esp_err_t err = esp_partition_read(partition, offset, buf, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Partition read failed at offset %zu: %s", offset, esp_err_to_name(err));
            free(buf);
            mbedtls_sha256_free(&ctx);
            return err;
        }
        mbedtls_sha256_update(&ctx, buf, chunk);
        offset += chunk;

        // Update progress callback during verification
        if ((offset % (256 * 1024) == 0 || offset >= size)) {
            int verify_progress = (int)((offset * 100) / size);
            if (s_ota.progress_callback) {
                s_ota.progress_callback(verify_progress, "Verifying checksum...");
            }
        }
    }

    mbedtls_sha256_finish(&ctx, computed);
    mbedtls_sha256_free(&ctx);
    free(buf);

    if (memcmp(computed, expected, 32) != 0) {
        ESP_LOGE(TAG, "SHA256 mismatch!");
        ESP_LOG_BUFFER_HEX_LEVEL("expected", expected, 32, ESP_LOG_ERROR);
        ESP_LOG_BUFFER_HEX_LEVEL("computed", computed, 32, ESP_LOG_ERROR);
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "SHA256 verification passed");
    return ESP_OK;
}

esp_err_t ota_manager_install_update(ota_progress_cb_t progress_cb, ota_ui_cb_t ui_cb)
{
    if (!s_ota.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ota.state != OTA_STATE_UPDATE_AVAILABLE) {
        ESP_LOGE(TAG, "No update available (state=%s)", ota_state_to_string(s_ota.state));
        return ESP_ERR_INVALID_STATE;
    }

    // Check blockers
    const char *block_reason;
    if (ota_manager_is_blocked(&block_reason)) {
        ESP_LOGE(TAG, "OTA blocked: %s", block_reason);
        set_error(block_reason);
        return ESP_ERR_INVALID_STATE;
    }

    // Check WiFi
    if (ota_check_wifi_connected() != ESP_OK) {
        set_error("No WiFi connection");
        return ESP_ERR_NOT_FOUND;
    }

    s_ota.progress_callback = progress_cb;
    s_ota.ui_callback = ui_cb;
    s_ota.download_progress = 0;

    const esp_app_desc_t *current_app = esp_app_get_description();
    ESP_LOGI(TAG, "Starting OTA update: %s -> %s",
             current_app->version, s_ota.release_info.version);

    // Enter unified p3a OTA state
    esp_err_t state_err = p3a_state_enter_ota();
    if (state_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enter p3a OTA state: %s (continuing anyway)", esp_err_to_name(state_err));
    }

    // Update render state with version info
    p3a_render_set_ota_progress(0, "Preparing...", current_app->version, s_ota.release_info.version);

    // Enter UI mode to stop animations and free memory
    if (ui_cb) {
        ui_cb(true, current_app->version, s_ota.release_info.version);
        s_ota.ui_active = true;
    }

    set_progress(0, "Preparing...");

    // Wait for system to stabilize after UI mode transition
    ESP_LOGI(TAG, "Waiting for network to stabilize after UI mode transition...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Re-check WiFi after UI mode transition
    if (ota_check_wifi_connected() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi disconnected after UI mode transition");
        set_error("WiFi disconnected");
        set_progress(0, "WIFI ERROR!");
        vTaskDelay(pdMS_TO_TICKS(5000));
        ota_exit_ui_mode();
        set_state(OTA_STATE_ERROR);
        // Exit p3a OTA state and return to playback
        p3a_state_exit_to_playback();
        return ESP_ERR_NOT_FOUND;
    }

    // Download SHA256 first if available
    uint8_t expected_sha256[32] = {0};
    bool have_sha256 = false;
    bool sha256_required = strlen(s_ota.release_info.sha256_url) > 0;

    if (sha256_required) {
        set_progress(0, "Downloading checksum...");

        char sha256_hex[65];
        esp_err_t err = ESP_FAIL;

        // Retry SHA256 download up to 3 times with delays
        for (int retry = 0; retry < 3 && err != ESP_OK; retry++) {
            if (retry > 0) {
                ESP_LOGW(TAG, "Retrying SHA256 download (attempt %d/3)...", retry + 1);
                vTaskDelay(pdMS_TO_TICKS(2000));  // Wait before retry
            }
            err = github_ota_download_sha256(s_ota.release_info.sha256_url, sha256_hex, sizeof(sha256_hex));
        }

        if (err == ESP_OK) {
            err = github_ota_hex_to_bin(sha256_hex, expected_sha256);
            if (err == ESP_OK) {
                have_sha256 = true;
                ESP_LOGI(TAG, "SHA256 checksum downloaded successfully");
            } else {
                ESP_LOGE(TAG, "Failed to parse SHA256 hex string");
            }
        } else {
            ESP_LOGE(TAG, "Failed to download SHA256 checksum after 3 attempts");
        }

        if (!have_sha256) {
            // SHA256 was expected but failed - refuse to proceed
            ESP_LOGE(TAG, "Cannot verify firmware integrity - aborting update");
            set_error("Checksum download failed");
            set_progress(0, "CHECKSUM ERROR!");
            vTaskDelay(pdMS_TO_TICKS(5000));  // Show error for 5 seconds
            ota_exit_ui_mode();
            set_state(OTA_STATE_ERROR);
            // Exit p3a OTA state and return to playback
            p3a_state_exit_to_playback();
            return ESP_ERR_INVALID_CRC;
        }
    } else {
        ESP_LOGW(TAG, "No SHA256 URL provided, proceeding without checksum verification");
    }

    // Configure OTA
    set_state(OTA_STATE_DOWNLOADING);
    set_progress(0, "Connecting to server...");

    esp_http_client_config_t http_config = {
        .url = s_ota.release_info.download_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CONFIG_OTA_DOWNLOAD_TIMEOUT_SEC * 1000,
        .keep_alive_enable = true,
        .buffer_size = CONFIG_OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = 1024,
        .max_redirection_count = 5,  // GitHub redirects to CDN
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .partial_http_download = false,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        set_error("Failed to start download");
        return err;
    }

    // Get image info
    esp_app_desc_t new_app_info;
    err = esp_https_ota_get_img_desc(ota_handle, &new_app_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get image description: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        set_error("Invalid firmware image");
        return err;
    }

    ESP_LOGI(TAG, "New firmware: version=%s, project=%s", new_app_info.version, new_app_info.project_name);

    // Download with progress
    int total_size = esp_https_ota_get_image_size(ota_handle);
    ESP_LOGI(TAG, "Downloading %d bytes...", total_size);

    set_progress(0, "Downloading firmware...");

    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int downloaded = esp_https_ota_get_image_len_read(ota_handle);
        int percent = (total_size > 0) ? (downloaded * 100 / total_size) : 0;
        set_progress(percent, "Downloading...");
        // Update render state with version info
        p3a_render_set_ota_progress(percent, "Downloading...", current_app->version, s_ota.release_info.version);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        set_error("Download failed");
        ota_exit_ui_mode();
        // Exit p3a OTA state and return to playback
        p3a_state_exit_to_playback();
        return err;
    }

    // Check if image is valid
    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "Complete data was not received");
        esp_https_ota_abort(ota_handle);
        set_error("Incomplete download");
        set_progress(s_ota.download_progress, "DOWNLOAD ERROR!");
        vTaskDelay(pdMS_TO_TICKS(2000));
        ota_exit_ui_mode();
        // Exit p3a OTA state and return to playback
        p3a_state_exit_to_playback();
        return ESP_ERR_INVALID_SIZE;
    }

    set_state(OTA_STATE_FLASHING);
    set_progress(100, "Writing to flash...");

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        set_progress(100, "FLASH ERROR!");
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed");
            set_error("Image validation failed");
        } else {
            ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
            set_error("Flash write failed");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        ota_exit_ui_mode();
        // Exit p3a OTA state and return to playback
        p3a_state_exit_to_playback();
        return err;
    }

    // Verify SHA256 if we have it
    if (have_sha256) {
        set_state(OTA_STATE_VERIFYING);
        set_progress(0, "Verifying checksum...");

        const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
        if (update_partition) {
            err = ota_verify_partition_sha256(update_partition, total_size, expected_sha256);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "SHA256 verification failed!");
                set_progress(100, "VERIFY ERROR!");
                vTaskDelay(pdMS_TO_TICKS(2000));
                ota_exit_ui_mode();
                // Mark partition as invalid
                set_error("Checksum verification failed");
                // Exit p3a OTA state and return to playback
                p3a_state_exit_to_playback();
                return err;
            }
        }
    }

    set_state(OTA_STATE_PENDING_REBOOT);
    set_progress(100, "Update complete!");

    ESP_LOGI(TAG, "OTA update successful! Rebooting in 3 seconds...");

    vTaskDelay(pdMS_TO_TICKS(3000));

    // Note: We don't exit UI mode since we're rebooting immediately
    esp_restart();

    // Never reached
    return ESP_OK;
}

esp_err_t ota_manager_rollback(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(running);

    if (!other) {
        ESP_LOGE(TAG, "No rollback partition available");
        return ESP_ERR_NOT_FOUND;
    }

    // Check if there's a valid image in the other slot
    esp_app_desc_t other_app_info;
    esp_err_t err = esp_ota_get_partition_description(other, &other_app_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No valid image in rollback partition");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Rolling back from %s to %s",
             esp_app_get_description()->version, other_app_info.version);

    err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Rollback scheduled, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    // Never reached
    return ESP_OK;
}

esp_err_t ota_manager_validate_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGW(TAG, "Could not get running partition");
        return ESP_OK;  // Not an error, might be factory partition
    }

    esp_ota_img_states_t ota_state;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err != ESP_OK) {
        // Probably running from factory partition
        ESP_LOGI(TAG, "Running from non-OTA partition");
        return ESP_OK;
    }

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "New OTA firmware pending verification");

        // Run basic self-tests
        // For now, just check that we got this far successfully
        // In a production system, you might check LCD, WiFi, etc.

        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "OTA firmware validated successfully");
    } else if (ota_state == ESP_OTA_IMG_VALID) {
        ESP_LOGD(TAG, "Running validated OTA firmware");
    }

    return ESP_OK;
}
