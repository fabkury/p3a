/**
 * @file slave_ota.c
 * @brief ESP32-C6 Co-processor OTA Update via ESP-Hosted
 */

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

static const char *TAG = "slave_ota";

// Expected slave firmware version (must match the built slave firmware)
// Pinned to 2.7.0 exactly - see docs/slave-ota/ESP32-C6-OTA-COMPATIBILITY.md
static const uint32_t SLAVE_FW_VERSION_MAJOR = 2;
static const uint32_t SLAVE_FW_VERSION_MINOR = 7;
static const uint32_t SLAVE_FW_VERSION_PATCH = 0;

// Partition label for slave firmware
#define SLAVE_FW_PARTITION_LABEL "slave_fw"

// OTA write chunk size
#define OTA_CHUNK_SIZE 1400

esp_err_t slave_ota_get_embedded_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
    if (major) *major = SLAVE_FW_VERSION_MAJOR;
    if (minor) *minor = SLAVE_FW_VERSION_MINOR;
    if (patch) *patch = SLAVE_FW_VERSION_PATCH;
    return ESP_OK;
}

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
    
    ESP_LOGI(TAG, "Current co-processor firmware: %lu.%lu.%lu", 
             current_ver.major1, current_ver.minor1, current_ver.patch1);
    ESP_LOGI(TAG, "Embedded slave firmware: %d.%d.%d",
             SLAVE_FW_VERSION_MAJOR, SLAVE_FW_VERSION_MINOR, SLAVE_FW_VERSION_PATCH);
    
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
    
    // Find slave firmware partition
    const esp_partition_t *slave_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, SLAVE_FW_PARTITION_LABEL);
    
    if (!slave_partition) {
        ESP_LOGE(TAG, "Slave firmware partition '%s' not found", SLAVE_FW_PARTITION_LABEL);
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
        return err;
    }
    
    // Verify it's a valid ESP-IDF app
    if (app_desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "Invalid app descriptor magic (got 0x%lx, expected 0x%lx)",
                 app_desc.magic_word, (uint32_t)ESP_APP_DESC_MAGIC_WORD);
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Slave firmware in partition: %s v%s", app_desc.project_name, app_desc.version);
    
    // Get total firmware size by parsing image header (like official ESP-Hosted example)
    esp_image_header_t img_header;
    err = esp_partition_read(slave_partition, 0, &img_header, sizeof(img_header));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read image header: %s", esp_err_to_name(err));
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
            return err;
        }
        fw_size += sizeof(seg_header) + seg_header.data_len;
        offset += sizeof(seg_header) + seg_header.data_len;
    }
    
    // Add padding to align to 16 bytes
    size_t padding = (16 - (fw_size % 16)) % 16;
    fw_size += padding;
    
    // Add checksum byte (always present)
    fw_size += 1;
    
    // Add SHA256 hash if appended
    if (img_header.hash_appended == 1) {
        fw_size += 32;
    }
    
    ESP_LOGI(TAG, "Starting co-processor OTA update (%zu bytes)...", fw_size);
    
    // Begin OTA
    err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }
    
    // Allocate transfer buffer
    uint8_t *buffer = malloc(OTA_CHUNK_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate transfer buffer");
        esp_hosted_slave_ota_end();
        return ESP_ERR_NO_MEM;
    }
    
    // Transfer firmware in chunks (reset offset to 0 for actual transfer)
    size_t xfer_offset = 0;
    size_t bytes_written = 0;
    int progress_pct = 0;
    
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
        
        // Progress logging every 10%
        int new_pct = (bytes_written * 100) / fw_size;
        if (new_pct >= progress_pct + 10) {
            progress_pct = new_pct;
            ESP_LOGI(TAG, "OTA progress: %d%% (%zu bytes)", progress_pct, bytes_written);
        }
    }
    
    free(buffer);
    
    if (err != ESP_OK) {
        esp_hosted_slave_ota_end();
        return err;
    }
    
    ESP_LOGI(TAG, "Firmware transfer complete (%zu bytes), finalizing...", bytes_written);
    
    // End OTA (validates firmware)
    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Activating new co-processor firmware...");
    
    // Activate new firmware (triggers co-processor reboot)
    err = esp_hosted_slave_ota_activate();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_activate failed: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Co-processor firmware updated successfully!");
    ESP_LOGW(TAG, "System will restart to complete the update...");
    
    // Give some time for the co-processor to reboot
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // Restart the host to re-sync with updated co-processor
    esp_restart();
    
    return ESP_OK;  // Won't reach here due to restart
}

