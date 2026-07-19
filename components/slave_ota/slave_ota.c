// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file slave_ota.c
 * @brief ESP32-C6 Co-processor OTA Update via ESP-Hosted
 */

#include <stdio.h>
#include <string.h>
#include "slave_ota.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_hosted.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p3a_state.h"
#include "p3a_render.h"

static const char *TAG = "slave_ota";

// Expected slave firmware version (must match the built slave firmware)
static const uint32_t SLAVE_FW_VERSION_MAJOR = 2;
static const uint32_t SLAVE_FW_VERSION_MINOR = 9;
static const uint32_t SLAVE_FW_VERSION_PATCH = 3;

// Version 2.7.0 has a confirmed OTA bug that prevents ANY upgrade
// This was verified by testing both p3a and official esp_hosted examples.
// See: https://github.com/espressif/esp-hosted-mcu/issues/143
// Note: 0.0.0 -> 2.9.3 works fine; only 2.7.0 is affected.
static const uint32_t LOCKED_VERSION_MAJOR = 2;
static const uint32_t LOCKED_VERSION_MINOR = 7;
static const uint32_t LOCKED_VERSION_PATCH = 0;

// Partition label for slave firmware
#define SLAVE_FW_PARTITION_LABEL "slave_fw"

// OTA write chunk size
#define OTA_CHUNK_SIZE 1400

// Set true once we commit to running the OTA (after USB gate clears, before
// any UI is shown). Read from button ISR/timer, HTTP worker, MQTT handler.
// Single bool — no mutex required.
static volatile bool s_slave_ota_in_progress = false;

// USB-busy predicate registered by app_main. NULL means "don't check" —
// safe default: proceed with OTA. Decouples slave_ota from TinyUSB.
static slave_ota_usb_busy_fn_t s_usb_busy_check = NULL;

// Snapshot of the version the C6 reported at boot. Valid only when the query
// succeeded and returned non-0.0.0. A per-boot snapshot cannot go stale: the
// running slave version only changes via slave OTA, and every OTA path ends
// in esp_restart().
static uint32_t s_running_ver_major = 0;
static uint32_t s_running_ver_minor = 0;
static uint32_t s_running_ver_patch = 0;
static bool s_running_ver_valid = false;

// True when the C6 runs the 2.7.0 firmware whose OTA bug blocks any upgrade.
static bool s_version_locked = false;

bool slave_ota_is_in_progress(void)
{
    return s_slave_ota_in_progress;
}

void slave_ota_set_usb_busy_check(slave_ota_usb_busy_fn_t fn)
{
    s_usb_busy_check = fn;
}

esp_err_t slave_ota_get_embedded_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
    if (major) *major = SLAVE_FW_VERSION_MAJOR;
    if (minor) *minor = SLAVE_FW_VERSION_MINOR;
    if (patch) *patch = SLAVE_FW_VERSION_PATCH;
    return ESP_OK;
}

esp_err_t slave_ota_get_running_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
    if (!s_running_ver_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    if (major) *major = s_running_ver_major;
    if (minor) *minor = s_running_ver_minor;
    if (patch) *patch = s_running_ver_patch;
    return ESP_OK;
}

bool slave_ota_is_version_locked(void)
{
    return s_version_locked;
}

// --------- UI helpers ---------
//
// Slave OTA does not touch ugfx_ui directly. It calls into p3a_render, which
// forwards through weak symbols to ugfx_ui. This keeps the component
// dependency direction clean (slave_ota -> p3a_core, never -> main).

static void ui_enter_slave_ota(const char *version_from, const char *version_to)
{
    p3a_state_enter_ota();
    p3a_state_set_ota_substate(P3A_OTA_SLAVE_FLASHING);
    p3a_render_show_slave_ota(version_from, version_to);
}

static void ui_update_slave_ota(int percent, const char *status)
{
    p3a_render_update_slave_ota(percent, status);
}

static void ui_finish_success_and_reboot(void)
{
    p3a_state_set_ota_substate(P3A_OTA_PENDING_REBOOT);
    for (int i = 5; i >= 1; i--) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Rebooting in %d...", i);
        ui_update_slave_ota(100, buf);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "Restarting to complete co-processor update");
    esp_restart();
}

static void ui_finish_failure_and_continue(esp_err_t err)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "OTA failed: %s", esp_err_to_name(err));
    ui_update_slave_ota(0, buf);
    vTaskDelay(pdMS_TO_TICKS(3000));
    p3a_render_hide_slave_ota();
    p3a_state_exit_to_playback();
    s_slave_ota_in_progress = false;
}

// USB-HS gate: if a USB cable is plugged in and the host has enumerated
// us (USB-MSC may be exposing the SD card), skip the slave OTA. The C6
// and the microSD share the SDIO bus; running both at once risks
// corruption or, at minimum, very slow OTA progress.
//
// The actual TinyUSB predicate is registered by app_main via
// slave_ota_set_usb_busy_check (typically app_usb_is_stream_active). If
// no predicate is registered, treat USB as not busy and run the OTA.
//
// A 500 ms settle delay covers the common race where the cable was just
// plugged in and TinyUSB hasn't completed enumeration yet by the time
// we reach this gate from app_main.
static bool usb_gate_blocks(void)
{
    if (!s_usb_busy_check) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    return s_usb_busy_check();
}

#if CONFIG_P3A_SLAVE_OTA_MOCK
esp_err_t slave_ota_run_mock(void)
{
    // s_slave_ota_in_progress is already true by the time we get here.
    ESP_LOGW(TAG, "Mock slave OTA: duration %d ms", CONFIG_P3A_SLAVE_OTA_MOCK_DURATION_MS);
    ui_enter_slave_ota("0.0.0", "mock-2.9.3");
    const int total_ms = CONFIG_P3A_SLAVE_OTA_MOCK_DURATION_MS;
    const int step_ms = total_ms / 100;
    for (int pct = 1; pct <= 100; pct++) {
        ui_update_slave_ota(pct, "Updating co-processor...");
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
    ui_update_slave_ota(100, "Verifying...");
    vTaskDelay(pdMS_TO_TICKS(500));
    ui_finish_success_and_reboot();  // never returns
    return ESP_OK;
}
#endif

esp_err_t slave_ota_check_and_update(void)
{
    ESP_LOGI(TAG, "Checking ESP32-C6 co-processor firmware...");
    
    // Get current co-processor version
    esp_hosted_coprocessor_fwver_t current_ver = {0};
    esp_err_t err = esp_hosted_get_coprocessor_fwversion(&current_ver);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not get co-processor version: %s", esp_err_to_name(err));
        // Continue anyway - might be old firmware without version support
        current_ver.major1 = 0;
        current_ver.minor1 = 0;
        current_ver.patch1 = 0;
    }
    
    // Snapshot the reported version for status consumers (/api/status).
    // 0.0.0 means "query failed or ancient firmware" — not a real version.
    s_running_ver_major = current_ver.major1;
    s_running_ver_minor = current_ver.minor1;
    s_running_ver_patch = current_ver.patch1;
    s_running_ver_valid = (current_ver.major1 != 0 || current_ver.minor1 != 0 ||
                           current_ver.patch1 != 0);

    ESP_LOGI(TAG, "Current co-processor firmware: %lu.%lu.%lu",
             current_ver.major1, current_ver.minor1, current_ver.patch1);
    ESP_LOGI(TAG, "Embedded slave firmware: %lu.%lu.%lu",
             SLAVE_FW_VERSION_MAJOR, SLAVE_FW_VERSION_MINOR, SLAVE_FW_VERSION_PATCH);

    // Check for version 2.7.0 which has a confirmed OTA bug preventing ANY upgrade.
    // This was verified by testing both p3a AND official esp_hosted examples - both fail.
    // The bug is in the slave firmware's OTA validation, not in the host code.
    // Devices with 2.7.0 will continue operating with that version.
    if (current_ver.major1 == LOCKED_VERSION_MAJOR &&
        current_ver.minor1 == LOCKED_VERSION_MINOR &&
        current_ver.patch1 == LOCKED_VERSION_PATCH) {
        s_version_locked = true;
        ESP_LOGW(TAG, "Detected esp_hosted 2.7.0 - OTA not possible (confirmed bug)");
        ESP_LOGW(TAG, "See: https://github.com/espressif/esp-hosted-mcu/issues/143");
        ESP_LOGW(TAG, "Device will continue operating with 2.7.0 slave firmware");
        ESP_LOGI(TAG, "Proceeding without co-processor update");
        return ESP_OK;
    }

#if CONFIG_P3A_SLAVE_OTA_MOCK
    // Mock OTA branch — placed BEFORE the needs_update check so it runs on
    // every boot regardless of the C6's actual version (which is how this
    // harness is meant to be used: enable Kconfig, reboot to trigger,
    // disable Kconfig when done). Still honors the USB gate so the
    // skip-on-USB scenario can be tested with the mock.
    ESP_LOGW(TAG, "CONFIG_P3A_SLAVE_OTA_MOCK enabled");
    if (usb_gate_blocks()) {
        ESP_LOGW(TAG, "USB-HS active - skipping mock slave OTA");
        return ESP_OK;
    }
    s_slave_ota_in_progress = true;
    ESP_LOGW(TAG, "Running mock OTA");
    return slave_ota_run_mock();  // never returns (esp_restart)
#endif

    // Check if update is needed
    bool needs_update = false;
    if (current_ver.major1 < SLAVE_FW_VERSION_MAJOR) {
        needs_update = true;
    } else if (current_ver.major1 == SLAVE_FW_VERSION_MAJOR) {
        if (current_ver.minor1 < SLAVE_FW_VERSION_MINOR) {
            needs_update = true;
        } else if (current_ver.minor1 == SLAVE_FW_VERSION_MINOR) {
            if (current_ver.patch1 < SLAVE_FW_VERSION_PATCH) {
                needs_update = true;
            }
        }
    }
    
    // Force update if version is 0.0.0 (indicates old/corrupt firmware)
    if (current_ver.major1 == 0 && current_ver.minor1 == 0 && current_ver.patch1 == 0) {
        ESP_LOGW(TAG, "Co-processor reports version 0.0.0 - forcing update");
        needs_update = true;
    }
    
    if (!needs_update) {
        ESP_LOGI(TAG, "Co-processor firmware is up to date");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Co-processor firmware update required!");

    // USB-HS gate: skip the OTA entirely if a USB cable is plugged in and the
    // host has enumerated us — USB-MSC may be exposing the SD card, and SD +
    // C6 share the SDIO bus.
    if (usb_gate_blocks()) {
        ESP_LOGW(TAG, "USB-HS active - skipping slave OTA to avoid SDIO conflict");
        ESP_LOGW(TAG, "Unplug USB and reboot to apply co-processor update");
        return ESP_OK;
    }

    // Commit: from here on, input handlers (button, HTTP, MQTT) ignore the
    // user, and the renderer is about to be claimed.
    s_slave_ota_in_progress = true;

    // Build pre-OTA version strings for the UI.
    char version_from[32];
    char version_to[32];
    snprintf(version_from, sizeof(version_from), "%lu.%lu.%lu",
             current_ver.major1, current_ver.minor1, current_ver.patch1);
    snprintf(version_to, sizeof(version_to), "%lu.%lu.%lu",
             SLAVE_FW_VERSION_MAJOR, SLAVE_FW_VERSION_MINOR, SLAVE_FW_VERSION_PATCH);

    // Find slave firmware partition
    const esp_partition_t *slave_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, SLAVE_FW_PARTITION_LABEL);

    if (!slave_partition) {
        // Don't take the screen for this case — the caller's app_main treats
        // ESP_ERR_NOT_FOUND as "no embedded firmware, that's fine".
        ESP_LOGE(TAG, "Slave firmware partition '%s' not found", SLAVE_FW_PARTITION_LABEL);
        s_slave_ota_in_progress = false;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Found slave firmware partition: offset=0x%lx, size=0x%lx",
             slave_partition->address, slave_partition->size);

    // Read app header to get firmware size
    esp_app_desc_t app_desc;
    err = esp_partition_read(slave_partition,
                             sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
                             &app_desc, sizeof(app_desc));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read app descriptor: %s", esp_err_to_name(err));
        s_slave_ota_in_progress = false;
        return err;
    }

    // Verify it's a valid ESP-IDF app
    if (app_desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "Invalid app descriptor magic (got 0x%lx, expected 0x%lx)",
                 app_desc.magic_word, (uint32_t)ESP_APP_DESC_MAGIC_WORD);
        s_slave_ota_in_progress = false;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Slave firmware in partition: %s v%s", app_desc.project_name, app_desc.version);

    // Get total firmware size by parsing image header (like official ESP-Hosted example)
    esp_image_header_t img_header;
    err = esp_partition_read(slave_partition, 0, &img_header, sizeof(img_header));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read image header: %s", esp_err_to_name(err));
        s_slave_ota_in_progress = false;
        return err;
    }

    // Calculate firmware size by parsing all segments
    size_t fw_size = sizeof(esp_image_header_t);
    size_t offset = sizeof(esp_image_header_t);

    for (int i = 0; i < img_header.segment_count; i++) {
        esp_image_segment_header_t seg_header;
        err = esp_partition_read(slave_partition, offset, &seg_header, sizeof(seg_header));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read segment %d header: %s", i, esp_err_to_name(err));
            s_slave_ota_in_progress = false;
            return err;
        }
        fw_size += sizeof(seg_header) + seg_header.data_len;
        offset += sizeof(seg_header) + seg_header.data_len;
    }

    // ESP-IDF image format: SHA256 must start at 16-byte aligned address
    // Padding is added between content and checksum to achieve this alignment
    // Formula: align (content + checksum) to 16 bytes, then add SHA256
    fw_size = ((fw_size + 1) + 15) & ~15;  // Content + padding + checksum, aligned

    // Add SHA256 hash if appended
    if (img_header.hash_appended == 1) {
        fw_size += 32;
    }

    ESP_LOGI(TAG, "Starting co-processor OTA update (%zu bytes)...", fw_size);

    // Now claim the screen.
    ui_enter_slave_ota(version_from, version_to);
    ui_update_slave_ota(0, "Preparing...");

    // Begin OTA
    err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_begin failed: %s", esp_err_to_name(err));
        ui_finish_failure_and_continue(err);
        return err;
    }

    // Allocate transfer buffer
    uint8_t *buffer = malloc(OTA_CHUNK_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate transfer buffer");
        esp_hosted_slave_ota_end();
        ui_finish_failure_and_continue(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    // Transfer firmware in chunks
    size_t xfer_offset = 0;
    size_t bytes_written = 0;
    int progress_pct = 0;
    ui_update_slave_ota(0, "Updating co-processor...");

    while (xfer_offset < fw_size) {
        size_t chunk_size = (fw_size - xfer_offset) > OTA_CHUNK_SIZE ? OTA_CHUNK_SIZE : (fw_size - xfer_offset);

        err = esp_partition_read(slave_partition, xfer_offset, buffer, chunk_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Partition read failed at offset %zu: %s", xfer_offset, esp_err_to_name(err));
            break;
        }

        err = esp_hosted_slave_ota_write(buffer, chunk_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed at offset %zu: %s", xfer_offset, esp_err_to_name(err));
            break;
        }

        xfer_offset += chunk_size;
        bytes_written += chunk_size;

        // Update UI on every percent change; log every 10%.
        int new_pct = (bytes_written * 100) / fw_size;
        if (new_pct != progress_pct) {
            progress_pct = new_pct;
            ui_update_slave_ota(progress_pct, "Updating co-processor...");
            if (progress_pct % 10 == 0) {
                ESP_LOGI(TAG, "OTA progress: %d%% (%zu bytes)", progress_pct, bytes_written);
            }
        }
    }

    free(buffer);

    if (err != ESP_OK) {
        esp_hosted_slave_ota_end();
        ui_finish_failure_and_continue(err);
        return err;
    }

    ESP_LOGI(TAG, "Firmware transfer complete (%zu bytes), finalizing...", bytes_written);
    ui_update_slave_ota(100, "Verifying...");

    // End OTA (validates firmware)
    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_end failed: %s", esp_err_to_name(err));
        ui_finish_failure_and_continue(err);
        return err;
    }

    ESP_LOGI(TAG, "OTA transfer completed successfully!");

    // Only call activate if current slave firmware >= 2.6.0
    // The activate API is only supported by slave firmware versions >= 2.6.0
    // For older versions (like 0.0.0 factory), we skip activate and just restart
    bool activate_supported = (current_ver.major1 > 2) ||
                              (current_ver.major1 == 2 && current_ver.minor1 >= 6);

    if (activate_supported) {
        ESP_LOGI(TAG, "Activating new co-processor firmware...");
        ui_update_slave_ota(100, "Activating...");
        err = esp_hosted_slave_ota_activate();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_hosted_slave_ota_activate failed: %s", esp_err_to_name(err));
            // Don't return error - OTA transfer was successful
            // The new firmware will be active after restart anyway
        }
    } else {
        ESP_LOGI(TAG, "Activate API not supported (requires v2.6.0+)");
        ESP_LOGI(TAG, "New firmware will be active after restart");
    }

    ESP_LOGI(TAG, "Co-processor firmware updated successfully!");
    ESP_LOGW(TAG, "System will restart to complete the update...");

    // Replace the previous silent 3-second wait with a visible countdown.
    ui_finish_success_and_reboot();  // never returns

    return ESP_OK;  // Unreachable
}

